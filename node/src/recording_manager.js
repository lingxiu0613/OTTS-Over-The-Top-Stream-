import fs from "fs";
import fsp from "fs/promises";
import os from "os";
import path from "path";
import { spawn } from "child_process";
import { buildRtmpUrl } from "./rtmp_url.js";

function sanitizeSegment(segment) {
  return String(segment || "").replace(/[^a-zA-Z0-9._-]/g, "_");
}

function splitStreamKey(streamKey) {
  return String(streamKey || "")
    .split("/")
    .map((segment) => segment.trim())
    .filter(Boolean)
    .map(sanitizeSegment);
}

function streamDir(rootDir, streamKey) {
  const parts = splitStreamKey(streamKey);
  return parts.length ? path.join(rootDir, ...parts) : rootDir;
}

function recordingId(streamKey, startedEpochMs, format) {
  const safeKey = splitStreamKey(streamKey).join("__") || "stream";
  return `${safeKey}_${startedEpochMs}.${format}`;
}

async function readTail(filePath, maxLines = 80) {
  try {
    const content = await fsp.readFile(filePath, "utf8");
    return content.split(/\r?\n/).filter(Boolean).slice(-maxLines);
  } catch {
    return [];
  }
}

async function walkFiles(rootDir) {
  const result = [];
  async function walk(currentDir) {
    let entries = [];
    try {
      entries = await fsp.readdir(currentDir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const entry of entries) {
      const fullPath = path.join(currentDir, entry.name);
      if (entry.isDirectory()) {
        await walk(fullPath);
      } else if (entry.isFile() && (entry.name.endsWith(".flv") || entry.name.endsWith(".mp4"))) {
        result.push(fullPath);
      }
    }
  }
  await walk(rootDir);
  return result;
}

export class RecordingManager {
  constructor(options = {}) {
    this.ffmpegBin = options.ffmpegBin || "ffmpeg";
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.rootDir = options.rootDir || path.join(os.tmpdir(), "otts_recordings");
    this.processes = new Map();
  }

  async ensureRoot() {
    await fsp.mkdir(this.rootDir, { recursive: true });
  }

  normalizeFormat(format) {
    const value = String(format || "flv").toLowerCase();
    return value === "mp4" ? "mp4" : "flv";
  }

  outputDir(streamKey) {
    return streamDir(this.rootDir, streamKey);
  }

  publicPath(recording) {
    const relative = path.relative(this.rootDir, recording.outputPath).split(path.sep).map(encodeURIComponent).join("/");
    return `/recordings/${relative}`;
  }

  getStatus(streamKey) {
    const proc = this.processes.get(streamKey);
    return {
      stream_key: streamKey,
      recording: Boolean(proc && !proc.exited),
      running: Boolean(proc && !proc.exited),
      pid: proc?.child.pid || null,
      format: proc?.format || null,
      output_path: proc?.outputPath || null,
      public_path: proc ? this.publicPath(proc) : null,
      started_at: proc?.startedAt || null,
      started_epoch_ms: proc?.startedEpochMs || null,
      stopped_at: proc?.stoppedAt || null,
      stopped_epoch_ms: proc?.stoppedEpochMs || null,
      last_exit_code: proc?.lastExitCode ?? null,
      last_error: proc?.lastError || null,
      bytes: proc?.outputPath && fs.existsSync(proc.outputPath) ? fs.statSync(proc.outputPath).size : 0,
      log_path: proc?.logPath || null
    };
  }

  listStatuses(streams = []) {
    return streams.map((stream) => this.getStatus(stream.stream_key));
  }

  async start(streamKey, options = {}) {
    const normalizedKey = String(streamKey || "").trim();
    if (!normalizedKey) {
      throw new Error("missing stream_key");
    }
    const existing = this.processes.get(normalizedKey);
    if (existing && !existing.exited) {
      return this.getStatus(normalizedKey);
    }

    await this.ensureRoot();
    const format = this.normalizeFormat(options.format);
    const startedEpochMs = Date.now();
    const outputDir = this.outputDir(normalizedKey);
    await fsp.mkdir(outputDir, { recursive: true });
    const outputPath = path.join(outputDir, recordingId(normalizedKey, startedEpochMs, format));
    const logPath = path.join(outputDir, `${path.basename(outputPath)}.log`);
    const logStream = fs.createWriteStream(logPath, { flags: "a" });
    const inputUrl = buildRtmpUrl(this.rtmpBase, normalizedKey, "play");

    const outputArgs = format === "mp4"
      ? ["-c", "copy", "-movflags", "+faststart", "-f", "mp4", outputPath]
      : ["-c", "copy", "-f", "flv", outputPath];

    const child = spawn(
      this.ffmpegBin,
      [
        "-hide_banner",
        "-loglevel",
        "warning",
        "-y",
        "-i",
        inputUrl,
        ...outputArgs
      ],
      { stdio: ["ignore", "pipe", "pipe"] }
    );

    const state = {
      child,
      streamKey: normalizedKey,
      format,
      outputPath,
      logPath,
      exited: false,
      startedAt: new Date(startedEpochMs).toISOString(),
      startedEpochMs,
      stoppedAt: null,
      stoppedEpochMs: null,
      lastExitCode: null,
      lastError: null,
      logStream
    };

    child.stdout.on("data", (chunk) => logStream.write(chunk));
    child.stderr.on("data", (chunk) => logStream.write(chunk));
    child.on("error", (error) => {
      state.lastError = error.message;
    });
    child.on("exit", (code, signal) => {
      state.exited = true;
      state.stoppedEpochMs = Date.now();
      state.stoppedAt = new Date(state.stoppedEpochMs).toISOString();
      state.lastExitCode = code;
      if (signal) {
        state.lastError = `signal:${signal}`;
      }
      logStream.end();
    });

    this.processes.set(normalizedKey, state);
    return this.getStatus(normalizedKey);
  }

  async waitForExit(child, timeoutMs = 5000) {
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

  async stop(streamKey) {
    const normalizedKey = String(streamKey || "").trim();
    const proc = this.processes.get(normalizedKey);
    if (!proc) {
      return null;
    }
    if (!proc.exited) {
      proc.child.kill("SIGINT");
      await this.waitForExit(proc.child);
      if (!proc.exited) {
        proc.child.kill("SIGTERM");
        await this.waitForExit(proc.child, 2000);
      }
    }
    return this.getStatus(normalizedKey);
  }

  async listFiles() {
    await this.ensureRoot();
    const files = await walkFiles(this.rootDir);
    const result = [];
    for (const filePath of files) {
      try {
        const stat = await fsp.stat(filePath);
        const relative = path.relative(this.rootDir, filePath);
        const parts = relative.split(path.sep);
        const fileName = parts.pop();
        const streamKey = parts.join("/");
        result.push({
          id: relative.split(path.sep).join("/"),
          stream_key: streamKey,
          file_name: fileName,
          format: path.extname(fileName).replace(/^\./, ""),
          path: filePath,
          public_path: `/recordings/${relative.split(path.sep).map(encodeURIComponent).join("/")}`,
          bytes: stat.size,
          created_at: stat.birthtime.toISOString(),
          modified_at: stat.mtime.toISOString(),
          modified_epoch_ms: stat.mtimeMs
        });
      } catch {
        // ignore files that disappear during listing
      }
    }
    result.sort((a, b) => b.modified_epoch_ms - a.modified_epoch_ms);
    return result;
  }

  async readLogTail(streamKey, maxLines = 80) {
    const proc = this.processes.get(streamKey);
    if (!proc?.logPath) {
      return [];
    }
    return readTail(proc.logPath, maxLines);
  }
}
