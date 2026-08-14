import fs from "fs";
import fsp from "fs/promises";
import os from "os";
import path from "path";
import { spawn } from "child_process";
import { URLSearchParams } from "url";

async function readTail(filePath, maxLines = 80) {
  try {
    const content = await fsp.readFile(filePath, "utf8");
    return content.split(/\r?\n/).filter(Boolean).slice(-maxLines);
  } catch {
    return [];
  }
}

function sanitizeStreamKey(streamKey) {
  return String(streamKey || "").replace(/[^a-zA-Z0-9/_-]/g, "_");
}

function detectPublicHost() {
  const interfaces = os.networkInterfaces();
  for (const entries of Object.values(interfaces)) {
    for (const entry of entries || []) {
      if (!entry || entry.internal || entry.family !== "IPv4") {
        continue;
      }
      if (entry.address.startsWith("127.")) {
        continue;
      }
      return entry.address;
    }
  }
  return "127.0.0.1";
}

export class SrtManager {
  constructor(options = {}) {
    this.ffmpegBin = options.ffmpegBin || "ffmpeg";
    this.ffprobeBin = options.ffprobeBin || "ffprobe";
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.coreApiBase = options.coreApiBase || "http://127.0.0.1:8080";
    this.logDir = options.logDir || path.join(os.tmpdir(), "otts_srt");
    this.publicHost = options.publicHost || detectPublicHost();
    this.publishPortBase = Number(options.publishPortBase || 9000);
    this.playPortBase = Number(options.playPortBase || 10000);
    this.restartDelayMs = Number(options.restartDelayMs || 1500);
    this.sessions = new Map();
  }

  async waitForExit(child, timeoutMs = 3000) {
    if (!child || child.exitCode !== null) {
      return;
    }
    await new Promise((resolve) => {
      let settled = false;
      const finish = () => {
        if (settled) {
          return;
        }
        settled = true;
        resolve();
      };
      const timer = setTimeout(finish, timeoutMs);
      child.once("exit", () => {
        clearTimeout(timer);
        finish();
      });
    });
  }

  async ensureLogDir() {
    await fsp.mkdir(this.logDir, { recursive: true });
  }

  assignPorts(streamKey) {
    const sorted = [...this.sessions.keys(), streamKey].filter((value, index, list) => list.indexOf(value) === index).sort();
    const index = Math.max(0, sorted.indexOf(streamKey));
    return {
      publishPort: this.publishPortBase + index * 2,
      playPort: this.playPortBase + index * 2
    };
  }

  publishUrl(host, port) {
    return `srt://${host}:${port}?mode=caller&transtype=live`;
  }

  playUrl(host, port) {
    return `srt://${host}:${port}?mode=caller&transtype=live`;
  }

  logPath(streamKey, mode) {
    return path.join(this.logDir, `${sanitizeStreamKey(streamKey).replaceAll("/", "__")}_${mode}.log`);
  }

  targetRtmpUrl(streamKey) {
    return `${this.rtmpBase}/${streamKey}`;
  }

  getOrCreateSession(streamKey) {
    let session = this.sessions.get(streamKey);
    if (!session) {
      const ports = this.assignPorts(streamKey);
      session = {
        streamKey,
        publishPort: ports.publishPort,
        playPort: ports.playPort,
        desiredPublish: false,
        desiredPlay: false,
        publish: null,
        play: null
      };
      this.sessions.set(streamKey, session);
    }
    return session;
  }

  getSession(streamKey) {
    return this.sessions.get(streamKey) || null;
  }

  processStatus(streamKey, mode, state) {
    return {
      mode,
      running: Boolean(state && !state.exited),
      pid: state?.child.pid || null,
      started_at: state?.startedAt || null,
      started_epoch_ms: state?.startedEpochMs || null,
      last_stopped_at: state?.lastStoppedAt || null,
      last_stopped_epoch_ms: state?.lastStoppedEpochMs || null,
      last_exit_code: state?.lastExitCode ?? null,
      last_error: state?.lastError || null,
      restart_count: state?.restartCount ?? 0,
      log_path: this.logPath(streamKey, mode)
    };
  }

  maybeDropSession(streamKey) {
    const session = this.sessions.get(streamKey);
    if (!session) {
      return;
    }
    const publishRunning = Boolean(session.publish && !session.publish.exited);
    const playRunning = Boolean(session.play && !session.play.exited);
    if (!session.desiredPublish && !session.desiredPlay && !publishRunning && !playRunning) {
      this.sessions.delete(streamKey);
    }
  }

  statusFor(streamKey) {
    const session = this.getOrCreateSession(streamKey);
    return this.buildStatus(streamKey, session);
  }

  buildStatus(streamKey, session) {
    return {
      stream_key: streamKey,
      publish_url: this.publishUrl(this.publicHost, session.publishPort),
      play_url: this.playUrl(this.publicHost, session.playPort),
      publish_listener_url: `srt://0.0.0.0:${session.publishPort}?mode=listener&transtype=live`,
      play_listener_url: `srt://0.0.0.0:${session.playPort}?mode=listener&transtype=live`,
      target_rtmp_url: this.targetRtmpUrl(streamKey),
      desired_publish: session.desiredPublish,
      desired_play: session.desiredPlay,
      publish_port: session.publishPort,
      play_port: session.playPort,
      publish: this.processStatus(streamKey, "publish", session.publish),
      play: this.processStatus(streamKey, "play", session.play)
    };
  }

  maybeStatusFor(streamKey) {
    const session = this.sessions.get(streamKey);
    if (!session) {
      return null;
    }
    return this.buildStatus(streamKey, session);
  }

  listStatuses() {
    return [...this.sessions.keys()].sort().map((streamKey) => this.buildStatus(streamKey, this.sessions.get(streamKey)));
  }

  async syncCoreStream(streamKey, options = {}) {
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "srt"
    });
    if (options.audioCodec) {
      params.set("audio_codec", options.audioCodec);
    }
    if (options.videoCodec) {
      params.set("video_codec", options.videoCodec);
    }
    if (options.managedBy) {
      params.set("managed_by", options.managedBy);
    }
    if (typeof options.hasPublisher === "boolean") {
      params.set("has_publisher", options.hasPublisher ? "true" : "false");
    }
    const endpoint = options.remove
      ? `/api/internal/streams/remove?${params.toString()}`
      : `/api/internal/streams/upsert?${params.toString()}`;
    try {
      await fetch(`${this.coreApiBase}${endpoint}`, { method: "POST" });
    } catch {
      // best effort
    }
  }

  async startPublish(streamKey) {
    const normalizedKey = String(streamKey || "").trim();
    if (!normalizedKey) {
      throw new Error("missing stream_key");
    }
    await this.ensureLogDir();
    const session = this.getOrCreateSession(normalizedKey);
    session.desiredPublish = true;
    if (session.publish && !session.publish.exited) {
      return this.statusFor(normalizedKey);
    }

    const logPath = this.logPath(normalizedKey, "publish");
    const logStream = fs.createWriteStream(logPath, { flags: "a" });
    const targetUrl = this.targetRtmpUrl(normalizedKey);
    const child = spawn(
      this.ffmpegBin,
      [
        "-hide_banner",
        "-loglevel",
        "warning",
        "-fflags",
        "+genpts",
        "-i",
        `srt://0.0.0.0:${session.publishPort}?mode=listener&transtype=live`,
        "-c",
        "copy",
        "-f",
        "flv",
        targetUrl
      ],
      { stdio: ["ignore", "pipe", "pipe"] }
    );

    session.publish = {
      child,
      exited: false,
      startedAt: new Date().toISOString(),
      startedEpochMs: Date.now(),
      lastStoppedAt: session.publish?.lastStoppedAt || null,
      lastStoppedEpochMs: session.publish?.lastStoppedEpochMs || null,
      lastExitCode: null,
      lastError: null,
      restartCount: session.publish?.restartCount || 0,
      logStream
    };

    await this.syncCoreStream(normalizedKey, {
      videoCodec: "h264",
      audioCodec: "aac",
      managedBy: "node-srt-publish",
      hasPublisher: true
    });

    child.stdout.on("data", (chunk) => logStream.write(chunk));
    child.stderr.on("data", (chunk) => logStream.write(chunk));
    child.on("error", (error) => {
      session.publish.lastError = error.message;
    });
    child.on("exit", async (code, signal) => {
      session.publish.exited = true;
      session.publish.lastStoppedAt = new Date().toISOString();
      session.publish.lastStoppedEpochMs = Date.now();
      session.publish.lastExitCode = code;
      if (signal) {
        session.publish.lastError = `signal:${signal}`;
      }
      logStream.end();
      await this.syncCoreStream(normalizedKey, { remove: true });
      if (session.desiredPublish) {
        session.publish.restartCount = (session.publish.restartCount || 0) + 1;
        setTimeout(() => {
          this.startPublish(normalizedKey).catch(() => {
            // best effort auto-restart
          });
        }, this.restartDelayMs);
      }
      this.maybeDropSession(normalizedKey);
    });

    return this.statusFor(normalizedKey);
  }

  async startPlay(streamKey) {
    const normalizedKey = String(streamKey || "").trim();
    if (!normalizedKey) {
      throw new Error("missing stream_key");
    }
    await this.ensureLogDir();
    const session = this.getOrCreateSession(normalizedKey);
    session.desiredPlay = true;
    if (session.play && !session.play.exited) {
      return this.statusFor(normalizedKey);
    }

    const logPath = this.logPath(normalizedKey, "play");
    const logStream = fs.createWriteStream(logPath, { flags: "a" });
    const child = spawn(
      this.ffmpegBin,
      [
        "-hide_banner",
        "-loglevel",
        "warning",
        "-re",
        "-i",
        this.targetRtmpUrl(normalizedKey),
        "-c",
        "copy",
        "-f",
        "mpegts",
        `srt://0.0.0.0:${session.playPort}?mode=listener&transtype=live`
      ],
      { stdio: ["ignore", "pipe", "pipe"] }
    );

    session.play = {
      child,
      exited: false,
      startedAt: new Date().toISOString(),
      startedEpochMs: Date.now(),
      lastStoppedAt: session.play?.lastStoppedAt || null,
      lastStoppedEpochMs: session.play?.lastStoppedEpochMs || null,
      lastExitCode: null,
      lastError: null,
      restartCount: session.play?.restartCount || 0,
      logStream
    };

    child.stdout.on("data", (chunk) => logStream.write(chunk));
    child.stderr.on("data", (chunk) => logStream.write(chunk));
    child.on("error", (error) => {
      session.play.lastError = error.message;
    });
    child.on("exit", (code, signal) => {
      session.play.exited = true;
      session.play.lastStoppedAt = new Date().toISOString();
      session.play.lastStoppedEpochMs = Date.now();
      session.play.lastExitCode = code;
      if (signal) {
        session.play.lastError = `signal:${signal}`;
      }
      logStream.end();
      if (session.desiredPlay) {
        session.play.restartCount = (session.play.restartCount || 0) + 1;
        setTimeout(() => {
          this.startPlay(normalizedKey).catch(() => {
            // best effort auto-restart
          });
        }, this.restartDelayMs);
      }
      this.maybeDropSession(normalizedKey);
    });

    return this.statusFor(normalizedKey);
  }

  async stop(streamKey, mode = "both") {
    const session = this.sessions.get(streamKey);
    if (!session) {
      return false;
    }
    if (mode === "both" || mode === "publish") {
      session.desiredPublish = false;
      if (session.publish && !session.publish.exited) {
        session.publish.child.kill("SIGTERM");
        await this.waitForExit(session.publish.child);
      }
      await this.syncCoreStream(streamKey, { remove: true });
    }
    if (mode === "both" || mode === "play") {
      session.desiredPlay = false;
      if (session.play && !session.play.exited) {
        session.play.child.kill("SIGTERM");
        await this.waitForExit(session.play.child);
      }
    }
    this.maybeDropSession(streamKey);
    return true;
  }

  async readLogTail(streamKey, mode = "publish", maxLines = 80) {
    return readTail(this.logPath(streamKey, mode), maxLines);
  }
}
