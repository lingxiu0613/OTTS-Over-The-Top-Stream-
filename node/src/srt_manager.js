import fs from "fs";
import http from "http";
import https from "https";
import fsp from "fs/promises";
import os from "os";
import path from "path";
import { spawn } from "child_process";
import { URLSearchParams } from "url";
import { appendSrtPassphrase, buildHttpFlvUrl, buildRtmpUrl, isValidSrtPassphrase, streamToken } from "./rtmp_url.js";

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
    this.coreHttpBase = options.coreHttpBase || this.coreApiBase;
    this.logDir = options.logDir || path.join(os.tmpdir(), "otts_srt");
    this.publicHost = options.publicHost || detectPublicHost();
    this.publishPortBase = Number(options.publishPortBase || 9000);
    this.playPortBase = Number(options.playPortBase || 10000);
    this.restartDelayMs = Number(options.restartDelayMs || 1500);
    this.defaultPublishMode = options.defaultPublishMode || "core-direct-flv";
    this.defaultPlayMode = options.defaultPlayMode || "core-egress-flv";
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

  listenerUrl(port) {
    return appendSrtPassphrase(`srt://0.0.0.0:${port}?mode=listener&transtype=live`);
  }

  srtAuthEnabled() {
    return isValidSrtPassphrase(streamToken());
  }

  logPath(streamKey, mode) {
    return path.join(this.logDir, `${sanitizeStreamKey(streamKey).replaceAll("/", "__")}_${mode}.log`);
  }

  targetRtmpUrl(streamKey) {
    return `${this.rtmpBase}/${streamKey}`;
  }

  internalTargetRtmpUrl(streamKey) {
    return buildRtmpUrl(this.rtmpBase, streamKey, "publish");
  }

  coreFlvUrl(streamKey) {
    return buildHttpFlvUrl(this.coreHttpBase, streamKey, "play");
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
          preferredPlayMode: this.defaultPlayMode,
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
      srt_auth_enabled: this.srtAuthEnabled(),
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
    const workerMode = mode === "publish"
      ? (worker?.ingestMode || targetSession.preferredPublishMode || this.defaultPublishMode)
      : (worker?.egressMode || targetSession.preferredPlayMode || this.defaultPlayMode);
    const isCoreDirect = mode === "publish" && workerMode === "core-direct-flv";
    const isCoreEgress = mode === "play" && workerMode === "core-egress-flv";
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
      target_url: isCoreDirect
        ? `${this.coreApiBase}/api/internal/media/publish/flv-stream`
        : (isCoreEgress ? this.coreFlvUrl(streamKey) : this.targetRtmpUrl(streamKey)),
      transport: "srt/mpegts",
      media_path: isCoreDirect
        ? "srt-ffmpeg-flv-stream-core-parser"
        : (isCoreEgress ? "core-http-flv-ffmpeg-srt" : "srt-ffmpeg-rtmp-bridge"),
      native_stage: isCoreDirect ? "core-direct" : (isCoreEgress ? "core-egress" : "compat-bridge"),
      codec_hint: "h264/aac passthrough",
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
      message_stream_id: String(message.messageStreamId || 1),
      payload_encoding: "raw"
    });
    const payload = Buffer.from(message.payload || []);
    if (!payload.length) {
      return;
    }
    const response = await fetch(`${this.coreApiBase}/api/internal/media/publish?${params.toString()}`, {
      method: "POST",
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Length": String(payload.length)
      },
      body: payload
    });
    if (!response.ok) {
      throw new Error(`core media publish failed: ${response.status} ${await response.text()}`);
    }
  }

  async publishFlvChunkToCore(streamKey, chunk) {
    const payload = Buffer.from(chunk || []);
    if (!payload.length) {
      return { parsed_tags: 0, media_packets: 0, media_bytes: 0, last_media_timestamp: 0 };
    }
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "srt",
      managed_by: "node-srt-publish"
    });
    const response = await fetch(`${this.coreApiBase}/api/internal/media/publish/flv-chunk?${params.toString()}`, {
      method: "POST",
      headers: {
        "Content-Type": "application/octet-stream",
        "Content-Length": String(payload.length)
      },
      body: payload
    });
    if (!response.ok) {
      throw new Error(`core flv chunk publish failed: ${response.status} ${await response.text()}`);
    }
    return response.json();
  }


  openFlvStreamToCore(streamKey) {
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "srt",
      managed_by: "node-srt-publish"
    });
    const target = new URL(`${this.coreApiBase}/api/internal/media/publish/flv-stream?${params.toString()}`);
    const client = target.protocol === "https:" ? https : http;
    const request = client.request(target, {
      method: "POST",
      headers: {
        "Content-Type": "application/octet-stream",
        "Transfer-Encoding": "chunked"
      }
    });
    const done = new Promise((resolve, reject) => {
      request.on("response", (response) => {
        const chunks = [];
        response.on("data", (chunk) => chunks.push(chunk));
        response.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          if (response.statusCode < 200 || response.statusCode >= 300) {
            reject(new Error(`core flv stream failed: ${response.statusCode} ${text}`));
            return;
          }
          try {
            resolve(JSON.parse(text || "{}"));
          } catch (error) {
            reject(error);
          }
        });
      });
      request.on("error", reject);
    });
    return { request, done };
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
    let coreStream = null;
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
            this.listenerUrl(session.publishPort),
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
            this.listenerUrl(session.publishPort),
            "-c",
            "copy",
            "-f",
            "flv",
            this.internalTargetRtmpUrl(normalizedKey)
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
        ingestMode: publishMode
      };

    await this.syncCoreStream(normalizedKey, {
      videoCodec: "h264",
      audioCodec: "aac",
      managedBy: "node-srt-publish",
      hasPublisher: true
    });
    await this.syncCoreSession(normalizedKey, "publish", "running", session.publish, session);

    if (publishMode === "core-direct-flv") {
      coreStream = this.openFlvStreamToCore(normalizedKey);
      coreStream.done
        .then((result) => {
          session.publish.parsedTags += Number(result.parsed_tags || 0);
          session.publish.mediaPackets += Number(result.media_packets || 0);
          session.publish.mediaBytes += Number(result.media_bytes || 0);
          if (result.last_media_timestamp !== undefined) {
            session.publish.lastMediaTimestamp = Number(result.last_media_timestamp || 0);
          }
        })
        .catch((error) => {
          session.publish.lastError = error instanceof Error ? error.message : String(error);
          if (!session.publish.exited && child && !child.killed) {
            child.kill("SIGTERM");
          }
        });
    }

    child.stdout.on("data", (chunk) => {
      session.publish.stdoutChunks += 1;
      session.publish.stdoutBytes += chunk.length;
      if (publishMode !== "core-direct-flv") {
        logStream.write(chunk);
        return;
      }
      if (coreStream) {
        const canContinue = coreStream.request.write(chunk);
        if (!canContinue) {
          child.stdout.pause();
          coreStream.request.once("drain", () => child.stdout.resume());
        }
      }
    });
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
      if (coreStream) {
        coreStream.request.end();
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
    const playMode = session.preferredPlayMode || this.defaultPlayMode;
    const inputUrl = playMode === "core-egress-flv"
      ? this.coreFlvUrl(normalizedKey)
      : this.internalTargetRtmpUrl(normalizedKey);
    const child = spawn(
      this.ffmpegBin,
      [
        "-hide_banner",
        "-loglevel",
        "warning",
        "-re",
        "-i",
        inputUrl,
        "-c",
        "copy",
        "-f",
        "mpegts",
        this.listenerUrl(session.playPort)
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
      egressMode: playMode,
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
