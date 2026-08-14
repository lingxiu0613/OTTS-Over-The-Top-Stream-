import os from "os";
import { URLSearchParams } from "url";

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

function sanitizeName(value) {
  return String(value || "").replace(/[^a-zA-Z0-9._/-]/g, "_");
}

export class RtspCompatManager {
  constructor(options = {}) {
    this.publicHost = options.publicHost || detectPublicHost();
    this.publishPort = options.publishPort || 8554;
    this.playPort = options.playPort || 8556;
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.coreApiBase = options.coreApiBase || "http://127.0.0.1:8080";
    this.desiredModes = new Map();
  }

  buildMount(streamKey) {
    const safe = sanitizeName(streamKey).replaceAll("/", "__");
    return safe.endsWith(".sdp") ? safe : `${safe}.sdp`;
  }

  publishUrl(host, streamKey) {
    return `rtsp://${host}:${this.publishPort}/${this.buildMount(streamKey)}`;
  }

  playUrl(host, streamKey) {
    return `rtsp://${host}:${this.playPort}/${this.buildMount(streamKey)}`;
  }

  targetRtmpUrl(streamKey) {
    return `${this.rtmpBase}/${streamKey}`;
  }

  sessionKey(streamKey, mode) {
    return `rtsp:${streamKey}:${mode}`;
  }

  markDesired(streamKey, mode, enabled) {
    const desired = this.desiredModes.get(streamKey) || { publish: false, play: false };
    desired[mode] = enabled;
    if (!desired.publish && !desired.play) {
      this.desiredModes.delete(streamKey);
      return;
    }
    this.desiredModes.set(streamKey, desired);
  }

  statusFor(streamKey) {
    const desired = this.desiredModes.get(streamKey) || {};
    return {
      stream_key: streamKey,
      publish_url: this.publishUrl(this.publicHost, streamKey),
      play_url: this.playUrl(this.publicHost, streamKey),
      target_rtmp_url: this.targetRtmpUrl(streamKey),
      desired_publish: Boolean(desired.publish),
      desired_play: Boolean(desired.play),
      publish: desired.publish
        ? {
            mode: "publish",
            running: true,
            pid: null,
            started_at: null,
            started_epoch_ms: null,
            last_exit_code: null,
            last_error: null,
            mount: this.buildMount(streamKey),
            port: this.publishPort,
            log_path: "native-rtsp-publish"
          }
        : null,
      play: desired.play
        ? {
            mode: "play",
            running: true,
            pid: null,
            started_at: null,
            started_epoch_ms: null,
            last_exit_code: null,
            last_error: null,
            mount: this.buildMount(streamKey),
            port: this.playPort,
            log_path: "native-rtsp-play"
          }
        : null
    };
  }

  maybeStatusFor(streamKey) {
    if (!this.desiredModes.has(streamKey)) {
      return null;
    }
    return this.statusFor(streamKey);
  }

  listStatuses() {
    return [...this.desiredModes.keys()].sort().map((streamKey) => this.statusFor(streamKey));
  }

  async syncCoreSession(streamKey, mode, state) {
    const params = new URLSearchParams({
      session_key: this.sessionKey(streamKey, mode),
      stream_key: streamKey,
      source_protocol: "rtsp",
      direction: mode,
      managed_by: `node-rtsp-${mode}`,
      state,
      public_url: mode === "publish"
        ? this.publishUrl(this.publicHost, streamKey)
        : this.playUrl(this.publicHost, streamKey),
      bind_url: mode === "publish"
        ? `rtsp://0.0.0.0:${this.publishPort}/${this.buildMount(streamKey)}`
        : `rtsp://0.0.0.0:${this.playPort}/${this.buildMount(streamKey)}`,
      target_url: this.targetRtmpUrl(streamKey),
      pid: "0",
      started_at_epoch_ms: "0",
      last_stopped_at_epoch_ms: "0",
      restart_count: "0",
      last_exit_code: "0",
      last_error: ""
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

  async startPublish(streamKey) {
    this.markDesired(streamKey, "publish", true);
    await this.syncCoreSession(streamKey, "publish", "ready");
    return this.statusFor(streamKey);
  }

  async startPlay(streamKey) {
    this.markDesired(streamKey, "play", true);
    await this.syncCoreSession(streamKey, "play", "ready");
    return this.statusFor(streamKey);
  }

  async stop(streamKey, mode = "both") {
    if (mode === "both" || mode === "publish") {
      this.markDesired(streamKey, "publish", false);
      await this.removeCoreSession(streamKey, "publish");
    }
    if (mode === "both" || mode === "play") {
      this.markDesired(streamKey, "play", false);
      await this.removeCoreSession(streamKey, "play");
    }
    return this.maybeStatusFor(streamKey);
  }

  async readLogTail() {
    return [];
  }
}
