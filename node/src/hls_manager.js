import fs from "fs";
import fsp from "fs/promises";
import os from "os";
import path from "path";
import { spawn } from "child_process";
import { buildRtmpUrl } from "./rtmp_url.js";

function sanitizeSegment(segment) {
  return segment.replace(/[^a-zA-Z0-9._-]/g, "_");
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
  if (!parts.length) {
    return rootDir;
  }
  return path.join(rootDir, ...parts);
}

async function rimrafDir(targetDir) {
  await fsp.rm(targetDir, { recursive: true, force: true });
}

async function readTail(filePath, maxLines = 80) {
  try {
    const content = await fsp.readFile(filePath, "utf8");
    return content.split(/\r?\n/).filter(Boolean).slice(-maxLines);
  } catch {
    return [];
  }
}

async function collectPlaylistDirs(rootDir) {
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
        continue;
      }
      if (entry.isFile() && entry.name === "index.m3u8") {
        result.push(path.dirname(fullPath));
      }
    }
  }

  await walk(rootDir);
  return result;
}

export class HlsManager {
  constructor(options = {}) {
    this.ffmpegBin = options.ffmpegBin || "ffmpeg";
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.rootDir = options.rootDir || path.join(os.tmpdir(), "otts_hls");
    this.autoStart = options.autoStart ?? true;
    this.idleStopMs = options.idleStopMs || 15000;
    this.cleanupAgeMs = options.cleanupAgeMs || 10 * 60 * 1000;
    this.processes = new Map();
  }

  async ensureRoot() {
    await fsp.mkdir(this.rootDir, { recursive: true });
  }

  playlistPath(streamKey) {
    return path.join(streamDir(this.rootDir, streamKey), "index.m3u8");
  }

  masterPlaylistPath(streamKey) {
    return path.join(streamDir(this.rootDir, streamKey), "master.m3u8");
  }

  segmentPath(streamKey, filename) {
    return path.join(streamDir(this.rootDir, streamKey), filename);
  }

  publicPath(streamKey) {
    return `/hls/${String(streamKey).split("/").map(encodeURIComponent).join("/")}/index.m3u8`;
  }

  masterPublicPath(streamKey) {
    return `/hls/${String(streamKey).split("/").map(encodeURIComponent).join("/")}/master.m3u8`;
  }

  logPath(streamKey) {
    return path.join(streamDir(this.rootDir, streamKey), "ffmpeg.log");
  }

  getStatus(streamKey) {
    const proc = this.processes.get(streamKey);
    const playlist = this.playlistPath(streamKey);
    const exists = fs.existsSync(playlist);
    let playlistMtimeEpochMs = null;
    let playlistAgeMs = null;
    if (exists) {
      try {
        const stat = fs.statSync(playlist);
        playlistMtimeEpochMs = stat.mtimeMs;
        playlistAgeMs = Date.now() - stat.mtimeMs;
      } catch {
        playlistMtimeEpochMs = null;
        playlistAgeMs = null;
      }
    }
    return {
      stream_key: streamKey,
      hls_path: this.publicPath(streamKey),
      hls_master_path: this.masterPublicPath(streamKey),
      playlist_path: playlist,
      master_playlist_path: this.masterPlaylistPath(streamKey),
      playlist_exists: exists,
      master_playlist_exists: fs.existsSync(this.masterPlaylistPath(streamKey)),
      playlist_mtime_epoch_ms: playlistMtimeEpochMs,
      playlist_age_ms: playlistAgeMs,
      running: Boolean(proc && !proc.exited),
      pid: proc?.child.pid || null,
      started_at: proc?.startedAt || null,
      last_seen_at: proc?.lastSeenAt || null,
      last_seen_epoch_ms: proc?.lastSeenEpochMs || null,
      last_exit_code: proc?.lastExitCode ?? null,
      last_error: proc?.lastError || null,
      restart_count: proc?.restartCount ?? 0,
      auto_start: this.autoStart
    };
  }

  async writeMasterPlaylist(streamKey) {
    await fsp.mkdir(streamDir(this.rootDir, streamKey), { recursive: true });
    const content = [
      "#EXTM3U",
      "#EXT-X-VERSION:3",
      '#EXT-X-STREAM-INF:BANDWIDTH=2500000,CODECS="avc1.64001f,mp4a.40.2"',
      "index.m3u8",
      ""
    ].join("\n");
    await fsp.writeFile(this.masterPlaylistPath(streamKey), content, "utf8");
  }

  listStatuses(streams = []) {
    return streams.map((stream) => this.getStatus(stream.stream_key));
  }

  async ensureRunning(streamKey) {
    await this.ensureRoot();
    const existing = this.processes.get(streamKey);
    if (existing && !existing.exited) {
      return this.getStatus(streamKey);
    }

    const outputDir = streamDir(this.rootDir, streamKey);
    await rimrafDir(outputDir);
    await fsp.mkdir(outputDir, { recursive: true });

    const playlistPath = path.join(outputDir, "index.m3u8");
    const segmentPattern = path.join(outputDir, "seg_%05d.ts");
    const logPath = path.join(outputDir, "ffmpeg.log");
    const logStream = fs.createWriteStream(logPath, { flags: "a" });
    const inputUrl = buildRtmpUrl(this.rtmpBase, streamKey, "play");

    const child = spawn(
      this.ffmpegBin,
      [
        "-hide_banner",
        "-loglevel",
        "warning",
        "-y",
        "-i",
        inputUrl,
        "-c",
        "copy",
        "-f",
        "hls",
        "-hls_time",
        "2",
        "-hls_list_size",
        "6",
        "-hls_flags",
        "delete_segments+append_list+omit_endlist+program_date_time",
        "-hls_segment_filename",
        segmentPattern,
        playlistPath
      ],
      {
        stdio: ["ignore", "pipe", "pipe"]
      }
    );

    const state = {
      child,
      exited: false,
      startedAt: new Date().toISOString(),
      startedEpochMs: Date.now(),
      lastSeenAt: new Date().toISOString(),
      lastSeenEpochMs: Date.now(),
      lastExitCode: null,
      lastError: null,
      restartCount: existing?.restartCount || 0,
      logStream
    };

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
    });

    this.processes.set(streamKey, state);
    await this.writeMasterPlaylist(streamKey);
    return this.getStatus(streamKey);
  }

  async waitForPlaylist(streamKey, timeoutMs = 6000) {
    const deadline = Date.now() + timeoutMs;
    const playlist = this.playlistPath(streamKey);

    while (Date.now() < deadline) {
      if (fs.existsSync(playlist)) {
        return true;
      }
      await new Promise((resolve) => setTimeout(resolve, 250));
    }

    return fs.existsSync(playlist);
  }

  async stop(streamKey) {
    const proc = this.processes.get(streamKey);
    if (!proc) {
      return false;
    }
    if (!proc.exited) {
      proc.child.kill("SIGTERM");
    }
    return true;
  }

  async readLogTail(streamKey, maxLines = 60) {
    return readTail(this.logPath(streamKey), maxLines);
  }

  markSeen(streamKey) {
    const proc = this.processes.get(streamKey);
    if (!proc) {
      return;
    }
    proc.lastSeenAt = new Date().toISOString();
    proc.lastSeenEpochMs = Date.now();
  }

  async syncStreams(streams = []) {
    await this.ensureRoot();
    const liveKeys = new Set();

    for (const stream of streams) {
      if (!stream?.stream_key) {
        continue;
      }
      if (!stream.has_publisher) {
        continue;
      }
      liveKeys.add(stream.stream_key);
      const existing = this.processes.get(stream.stream_key);
      if (this.autoStart && (!existing || existing.exited)) {
        if (existing?.exited) {
          existing.restartCount = (existing.restartCount || 0) + 1;
        }
        await this.ensureRunning(stream.stream_key);
      }
      this.markSeen(stream.stream_key);
    }

    const now = Date.now();
    for (const [streamKey, proc] of this.processes.entries()) {
      const stale = !liveKeys.has(streamKey) && proc.lastSeenEpochMs && now - proc.lastSeenEpochMs > this.idleStopMs;
      if (stale && !proc.exited) {
        proc.child.kill("SIGTERM");
      }
    }

    await this.cleanupStaleOutputs();
  }

  async cleanupStaleOutputs() {
    return this.cleanupStaleOutputsWithReport(false);
  }

  async cleanupStaleOutputsWithReport(includeRunning = false) {
    const removed = [];
    try {
      const playlistDirs = await collectPlaylistDirs(this.rootDir);
      const now = Date.now();
      const activeDirs = new Set(
        [...this.processes.entries()]
          .filter(([, proc]) => proc && !proc.exited)
          .map(([streamKey]) => streamDir(this.rootDir, streamKey))
      );

      for (const dir of playlistDirs) {
        if (!includeRunning && activeDirs.has(dir)) {
          continue;
        }
        const playlist = path.join(dir, "index.m3u8");
        const stat = await fsp.stat(playlist);
        const ageMs = now - stat.mtimeMs;
        if (ageMs > this.cleanupAgeMs) {
          await rimrafDir(dir);
          removed.push({ dir, age_ms: ageMs });
        }
      }
    } catch {
      // ignore cleanup errors
    }
    return removed;
  }
}
