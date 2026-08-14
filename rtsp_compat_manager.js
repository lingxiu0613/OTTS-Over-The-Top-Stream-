import os from "os";

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

  async startPublish(streamKey) {
    this.markDesired(streamKey, "publish", true);
    return this.statusFor(streamKey);
  }

  async startPlay(streamKey) {
    this.markDesired(streamKey, "play", true);
    return this.statusFor(streamKey);
  }

  async stop(streamKey, mode = "both") {
    if (mode === "both" || mode === "publish") {
      this.markDesired(streamKey, "publish", false);
    }
    if (mode === "both" || mode === "play") {
      this.markDesired(streamKey, "play", false);
    }
    return this.statusFor(streamKey);
  }

  async readLogTail() {
    return [];
  }
}
