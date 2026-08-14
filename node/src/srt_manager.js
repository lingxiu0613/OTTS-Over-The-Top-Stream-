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

function readBe24(buffer, offset) {
  return (buffer[offset] << 16) | (buffer[offset + 1] << 8) | buffer[offset + 2];
}

function createFlvTagParser(onTag) {
  let buffer = Buffer.alloc(0);
  let headerSkipped = false;

  return {
    push(chunk) {
      if (!chunk || !chunk.length) {
        return;
      }
      buffer = Buffer.concat([buffer, chunk]);

      if (!headerSkipped) {
        if (buffer.length < 13) {
          return;
        }
        if (buffer.subarray(0, 3).toString("ascii") !== "FLV") {
          throw new Error("invalid flv header from ffmpeg");
        }
        buffer = buffer.subarray(13);
        headerSkipped = true;
      }

      while (buffer.length >= 15) {
        const tagType = buffer[0];
        const dataSize = readBe24(buffer, 1);
        const timestamp = readBe24(buffer, 4) | (buffer[7] << 24);
        const totalSize = 11 + dataSize + 4;
        if (buffer.length < totalSize) {
          return;
        }
        const payload = buffer.subarray(11, 11 + dataSize);
        onTag({
          typeId: tagType,
          timestamp,
          messageStreamId: 1,
          payload: Buffer.from(payload)
        });
        buffer = buffer.subarray(totalSize);
      }
    }
  };
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
    this.defaultPublishMode = options.defaultPublishMode || "legacy-rtmp-loopback";
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

  sessionKey(streamKey, mode) {
    return `srt:${streamKey}:${mode}`;
  }

  getOrCreateSession(streamKey) {
    let session = this.sessions.get(streamKey);
    if (!session) {
      const ports = this.assignPorts(streamKey);
        session = {
          streamKey,
          publishPort: ports.publishPort,
          playPort: ports.playPort,
          preferredPublishMode: this.defaultPublishMode,
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
        ingest_mode: state?.ingestMode || "unknown",
        stdout_chunks: state?.stdoutChunks ?? 0,
        stdout_bytes: state?.stdoutBytes ?? 0,
        parsed_tags: state?.parsedTags ?? 0,
      media_packets: state?.mediaPackets ?? 0,
      media_bytes: state?.mediaBytes ?? 0,
      last_media_timestamp: state?.lastMediaTimestamp ?? 0,
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

  async syncCoreSession(streamKey, mode, state, worker = null, session = null) {
    const targetSession = session || this.getSession(streamKey);
    if (!targetSession) {
      return;
    }
    const params = new URLSearchParams({
      session_key: this.sessionKey(streamKey, mode),
      stream_key: streamKey,
      source_protocol: "srt",
      direction: mode,
      managed_by: `node-srt-${mode}`,
      state,
      public_url: mode === "publish"
        ? this.publishUrl(this.publicHost, targetSession.publishPort)
        : this.playUrl(this.publicHost, targetSession.playPort),
      bind_url: mode === "publish"
        ? `srt://0.0.0.0:${targetSession.publishPort}?mode=listener&transtype=live`
        : `srt://0.0.0.0:${targetSession.playPort}?mode=listener&transtype=live`,
      target_url: this.targetRtmpUrl(streamKey),
      pid: String(worker?.child?.pid || 0),
      started_at_epoch_ms: String(worker?.startedEpochMs || 0),
      last_stopped_at_epoch_ms: String(worker?.lastStoppedEpochMs || 0),
      restart_count: String(worker?.restartCount || 0),
      last_exit_code: String(worker?.lastExitCode ?? 0),
      last_error: worker?.lastError || ""
    });
    try {
      await fetch(`${this.coreApiBase}/api/internal/sessions/upsert?${params.toString()}`, { method: "POST" });
    } catch {
      // best effort
    }
  }

  async removeCoreSession(streamKey, mode) {
    const params = new URLSearchParams({
      session_key: this.sessionKey(streamKey, mode)
    });
    try {
      await fetch(`${this.coreApiBase}/api/internal/sessions/remove?${params.toString()}`, { method: "POST" });
    } catch {
      // best effort
    }
  }

  async publishMediaToCore(streamKey, message) {
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "srt",
      managed_by: "node-srt-publish",
      message_type: String(message.typeId),
      timestamp: String(message.timestamp || 0),
      message_stream_id: String(message.messageStreamId || 1)
    });
    const response = await fetch(`${this.coreApiBase}/api/internal/media/publish?${params.toString()}`, {
      method: "POST",
      headers: {
        "Content-Type": "text/plain"
      },
      body: Buffer.from(message.payload || []).toString("base64")
    });
    if (!response.ok) {
      throw new Error(`core media publish failed: ${response.status} ${await response.text()}`);
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
    const publishMode = session.preferredPublishMode || "core-direct-flv";

    const logPath = this.logPath(normalizedKey, "publish");
    const logStream = fs.createWriteStream(logPath, { flags: "a" });
    let publishChain = Promise.resolve();
    const parser = createFlvTagParser((message) => {
      session.publish.parsedTags += 1;
      publishChain = publishChain
        .then(async () => {
          await this.publishMediaToCore(normalizedKey, message);
          session.publish.mediaPackets += 1;
          session.publish.mediaBytes += Buffer.from(message.payload || []).length;
          session.publish.lastMediaTimestamp = message.timestamp || 0;
        })
        .catch((error) => {
          session.publish.lastError = error instanceof Error ? error.message : String(error);
        });
    });
    const child = spawn(
      this.ffmpegBin,
      publishMode === "core-direct-flv"
        ? [
            "-hide_banner",
            "-loglevel",
            "warning",
            "-fflags",
            "+genpts",
            "-i",
            `srt://0.0.0.0:${session.publishPort}?mode=listener&transtype=live`,
            "-c",
            "copy",
            "-flush_packets",
            "1",
            "-flvflags",
            "no_duration_filesize",
            "-f",
            "flv",
            "pipe:1"
          ]
        : [
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
            this.targetRtmpUrl(normalizedKey)
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
      stdoutChunks: 0,
      stdoutBytes: 0,
        parsedTags: 0,
        mediaPackets: 0,
        mediaBytes: 0,
        lastMediaTimestamp: 0,
        logStream,
        fallbackRequested: false,
        ingestMode: publishMode
      };

    await this.syncCoreStream(normalizedKey, {
      videoCodec: "h264",
      audioCodec: "aac",
      managedBy: "node-srt-publish",
      hasPublisher: true
    });
    await this.syncCoreSession(normalizedKey, "publish", "running", session.publish, session);

    child.stdout.on("data", (chunk) => {
      session.publish.stdoutChunks += 1;
      session.publish.stdoutBytes += chunk.length;
      if (publishMode !== "core-direct-flv") {
        logStream.write(chunk);
        return;
      }
      try {
        parser.push(chunk);
      } catch (error) {
        session.publish.lastError = error instanceof Error ? error.message : String(error);
        child.kill("SIGTERM");
      }
    });
    child.stderr.on("data", (chunk) => logStream.write(chunk));
    if (publishMode === "core-direct-flv") {
      setTimeout(() => {
        if (!session.publish || session.publish.exited) {
          return;
        }
        if ((session.publish.stdoutChunks || 0) > 0) {
          return;
        }
        session.publish.fallbackRequested = true;
        session.publish.lastError = "core-direct path produced no stdout, fallback to legacy rtmp loopback";
        session.preferredPublishMode = "legacy-rtmp-loopback";
        child.kill("SIGTERM");
      }, 5000);
    }
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
      await this.syncCoreSession(
        normalizedKey,
        "publish",
        session.desiredPublish ? "restarting" : "stopped",
        session.publish,
        session
      );
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

    await this.syncCoreSession(normalizedKey, "play", "running", session.play, session);

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
      this.syncCoreSession(
        normalizedKey,
        "play",
        session.desiredPlay ? "restarting" : "stopped",
        session.play,
        session
      ).catch(() => {
        // best effort
      });
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
      await this.removeCoreSession(streamKey, "publish");
    }
    if (mode === "both" || mode === "play") {
      session.desiredPlay = false;
      if (session.play && !session.play.exited) {
        session.play.child.kill("SIGTERM");
        await this.waitForExit(session.play.child);
      }
      await this.removeCoreSession(streamKey, "play");
    }
    this.maybeDropSession(streamKey);
    return true;
  }

  async readLogTail(streamKey, mode = "publish", maxLines = 80) {
    return readTail(this.logPath(streamKey, mode), maxLines);
  }
}
