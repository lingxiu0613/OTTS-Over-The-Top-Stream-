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

export class RtspProxyManager {
  constructor(options = {}) {
    this.ffmpegBin = options.ffmpegBin || "ffmpeg";
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.logDir = options.logDir || path.join(os.tmpdir(), "otts_rtsp");
    this.coreApiBase = options.coreApiBase || "http://127.0.0.1:8080";
    this.processes = new Map();
  }

  async ensureLogDir() {
    await fsp.mkdir(this.logDir, { recursive: true });
  }

  logPath(streamKey) {
    return path.join(this.logDir, `${sanitizeStreamKey(streamKey).replaceAll("/", "__")}.log`);
  }

  targetUrl(streamKey) {
    return `${this.rtmpBase}/${streamKey}`;
  }

  getStatus(streamKey) {
    const state = this.processes.get(streamKey);
    return {
      stream_key: streamKey,
      source_url: state?.sourceUrl || "",
      target_url: state?.targetUrl || this.targetUrl(streamKey),
      running: Boolean(state && !state.exited),
      pid: state?.child.pid || null,
      started_at: state?.startedAt || null,
      started_epoch_ms: state?.startedEpochMs || null,
      last_exit_code: state?.lastExitCode ?? null,
      last_error: state?.lastError || null,
      restart_count: state?.restartCount ?? 0,
      transport: state?.transport || "tcp",
      log_path: this.logPath(streamKey)
    };
  }

  listStatuses() {
    return [...this.processes.keys()].map((streamKey) => this.getStatus(streamKey));
  }

  async syncCoreStream(streamKey, options = {}) {
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "rtsp"
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

    const path = options.remove
      ? `/api/internal/streams/remove?${params.toString()}`
      : `/api/internal/streams/upsert?${params.toString()}`;

    try {
      await fetch(`${this.coreApiBase}${path}`, { method: "POST" });
    } catch {
      // best-effort sync only
    }
  }

  async start({ streamKey, sourceUrl, transport = "tcp" }) {
    const normalizedKey = String(streamKey || "").trim();
    const normalizedUrl = String(sourceUrl || "").trim();
    if (!normalizedKey) {
      throw new Error("missing stream_key");
    }
    if (!normalizedUrl) {
      throw new Error("missing source_url");
    }

    await this.ensureLogDir();

    const existing = this.processes.get(normalizedKey);
    if (existing && !existing.exited) {
      return this.getStatus(normalizedKey);
    }

    const logPath = this.logPath(normalizedKey);
    const logStream = fs.createWriteStream(logPath, { flags: "a" });
    const targetUrl = this.targetUrl(normalizedKey);
    const child = spawn(
      this.ffmpegBin,
      [
        "-hide_banner",
        "-loglevel",
        "warning",
        "-rtsp_transport",
        transport,
        "-i",
        normalizedUrl,
        "-c",
        "copy",
        "-f",
        "flv",
        targetUrl
      ],
      { stdio: ["ignore", "pipe", "pipe"] }
    );

    const state = {
      child,
      sourceUrl: normalizedUrl,
      targetUrl,
      transport,
      exited: false,
      startedAt: new Date().toISOString(),
      startedEpochMs: Date.now(),
      lastExitCode: null,
      lastError: null,
      restartCount: existing?.restartCount || 0,
      logStream
    };

    await this.syncCoreStream(normalizedKey, {
      videoCodec: "h264",
      audioCodec: "aac",
      managedBy: "node-rtsp-relay",
      hasPublisher: true
    });

    child.stdout.on("data", (chunk) => logStream.write(chunk));
    child.stderr.on("data", (chunk) => logStream.write(chunk));
    child.on("error", (error) => {
      state.lastError = error.message;
    });
    child.on("exit", (code, signal) => {
      state.exited = true;
      state.lastExitCode = code;
      if (signal) {
        state.lastError = `signal:${signal}`;
      }
      logStream.end();
      this.syncCoreStream(normalizedKey, { remove: true });
    });

    this.processes.set(normalizedKey, state);
    return this.getStatus(normalizedKey);
  }

  async stop(streamKey) {
    const state = this.processes.get(streamKey);
    if (!state) {
      return false;
    }
    if (!state.exited) {
      state.child.kill("SIGTERM");
    }
    await this.syncCoreStream(streamKey, { remove: true });
    return true;
  }

  async readLogTail(streamKey, maxLines = 60) {
    return readTail(this.logPath(streamKey), maxLines);
  }
}
