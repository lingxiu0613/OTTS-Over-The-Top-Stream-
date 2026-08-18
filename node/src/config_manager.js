import fs from "fs";
import fsp from "fs/promises";
import path from "path";
import { EventEmitter } from "events";

function parseBool(value, fallback = false) {
  if (value === undefined || value === null || value === "") {
    return fallback;
  }
  return !["0", "false", "no", "off"].includes(String(value).toLowerCase());
}

function parseNumber(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function deepMerge(base, override) {
  if (!override || typeof override !== "object" || Array.isArray(override)) {
    return override === undefined ? base : override;
  }
  const result = { ...(base || {}) };
  for (const [key, value] of Object.entries(override)) {
    if (value && typeof value === "object" && !Array.isArray(value)) {
      result[key] = deepMerge(result[key] || {}, value);
    } else {
      result[key] = value;
    }
  }
  return result;
}

function splitStreamKey(streamKey) {
  const parts = String(streamKey || "").split("/").filter(Boolean);
  return {
    vhost: "__default__",
    app: parts[0] || "live",
    stream: parts.slice(1).join("/") || parts[0] || "stream"
  };
}

function buildDefaultConfig(projectRoot) {
  return {
    version: 1,
    server: {
      publicHost: process.env.OTTS_PUBLIC_HOST || "192.168.40.11",
      rtspPublicHost: process.env.OTTS_RTSP_PUBLIC_HOST || process.env.OTTS_PUBLIC_HOST || "192.168.40.11",
      srtPublicHost: process.env.OTTS_SRT_PUBLIC_HOST || process.env.OTTS_PUBLIC_HOST || "192.168.40.11"
    },
    ports: {
      rtmp: parseNumber(process.env.OTTS_RTMP_PORT, 1935),
      httpApi: parseNumber(process.env.OTTS_HTTP_API_PORT, 8080),
      compatHttp: parseNumber(process.env.OTTS_COMPAT_HTTP_PORT, 1985),
      nodeHttp: parseNumber(process.env.PORT, 3000),
      nodeHttps: parseNumber(process.env.HTTPS_PORT, 3443),
      webrtcGateway: parseNumber(process.env.OTTS_WEBRTC_GATEWAY_PORT, 8081),
      rtspPublish: parseNumber(process.env.OTTS_RTSP_PUBLISH_PORT, 8554),
      rtspPlay: parseNumber(process.env.OTTS_RTSP_PLAY_PORT, 8556),
      srtPublish: parseNumber(process.env.OTTS_SRT_PUBLISH_PORT_BASE, 9000),
      srtPlay: parseNumber(process.env.OTTS_SRT_PLAY_PORT_BASE, 10000)
    },
    protocols: {
      rtmp: { enabled: true },
      flv: { enabled: true },
      hls: { enabled: true },
      rtsp: {
        enabled: true,
        publishMode: process.env.OTTS_RTSP_PUBLISH_MODE || "core-direct-flv",
        playMode: process.env.OTTS_RTSP_PLAY_MODE || "core-egress-flv"
      },
      srt: {
        enabled: true,
        publishMode: process.env.OTTS_SRT_PUBLISH_MODE || "core-direct-flv",
        playMode: process.env.OTTS_SRT_PLAY_MODE || "core-egress-flv"
      },
      webrtc: { enabled: true, mode: process.env.OTTS_WEBRTC_MODE || "auto" }
    },
    auth: {
      enabled: Boolean(process.env.OTTS_STREAM_TOKEN || process.env.OTTS_AUTH_SECRET),
      token: process.env.OTTS_STREAM_TOKEN || "",
      secret: process.env.OTTS_AUTH_SECRET || "",
      ttlSeconds: parseNumber(process.env.OTTS_AUTH_TTL_SECONDS, 3600)
    },
    recording: {
      rootDir: process.env.OTTS_RECORDING_ROOT || "/tmp/otts_recordings",
      defaultFormat: "flv",
      autoRecord: false,
      enabled: true
    },
    hls: {
      rootDir: process.env.OTTS_HLS_ROOT || path.join("/tmp", "otts_hls"),
      autoStart: parseBool(process.env.OTTS_HLS_AUTO_START, true),
      segmentSeconds: 2,
      listSize: 6,
      idleStopSeconds: 15,
      cleanupAgeSeconds: 600,
      restartBackoffSeconds: 5,
      playlistStaleSeconds: 10,
      playlistStartupTimeoutSeconds: 20
    },
    logging: {
      level: process.env.OTTS_LOG_LEVEL || "info",
      runtimeLog: "/tmp/otts_runtime.log"
    },
    callbacks: {
      enabled: false,
      timeoutMs: 3000,
      retries: 1,
      events: {
        on_publish: [],
        on_unpublish: [],
        on_play: [],
        on_stop: [],
        on_dvr: [],
        on_hls: []
      }
    },
    vhosts: [
      {
        name: "__default__",
        enabled: true,
        defaults: {},
        apps: [
          {
            name: "live",
            enabled: true,
            defaults: {},
            streams: []
          }
        ]
      }
    ],
    metadata: {
      projectRoot
    }
  };
}

function normalizeCallbacks(callbacks = {}) {
  const events = {};
  for (const name of ["on_publish", "on_unpublish", "on_play", "on_stop", "on_dvr", "on_hls"]) {
    const value = callbacks.events?.[name] ?? callbacks[name] ?? [];
    events[name] = Array.isArray(value) ? value : (value ? [value] : []);
  }
  return {
    enabled: Boolean(callbacks.enabled),
    timeoutMs: parseNumber(callbacks.timeoutMs, 3000),
    retries: parseNumber(callbacks.retries, 1),
    events
  };
}

export class ConfigManager extends EventEmitter {
  constructor(options = {}) {
    super();
    this.projectRoot = options.projectRoot || process.cwd();
    this.configPath = options.configPath || process.env.OTTS_CONFIG_JSON || path.join(this.projectRoot, "config", "otts.config.json");
    this.examplePath = options.examplePath || path.join(this.projectRoot, "config", "otts.config.json.example");
    this.config = buildDefaultConfig(this.projectRoot);
    this.loadedAt = null;
    this.lastError = null;
    this.watcher = null;
    this.reloadTimer = null;
  }

  async load() {
    const defaults = buildDefaultConfig(this.projectRoot);
    let raw = {};
    let source = this.configPath;
    try {
      const content = await fsp.readFile(this.configPath, "utf8");
      raw = JSON.parse(content);
    } catch (error) {
      if (error?.code === "ENOENT") {
        source = "defaults";
        raw = {};
      } else {
        this.lastError = error instanceof Error ? error.message : "invalid config";
        throw error;
      }
    }

    const merged = deepMerge(defaults, raw);
    merged.callbacks = normalizeCallbacks(merged.callbacks);
    merged.metadata = {
      ...(merged.metadata || {}),
      projectRoot: this.projectRoot,
      configPath: this.configPath,
      source,
      restartRequiredKeys: ["ports", "server.publicHost", "protocols.*.mode"]
    };
    this.config = merged;
    this.loadedAt = new Date().toISOString();
    this.lastError = null;
    this.emit("reload", this.getSnapshot());
    return this.config;
  }

  startWatching() {
    if (this.watcher) {
      return;
    }
    const dir = path.dirname(this.configPath);
    const fileName = path.basename(this.configPath);
    try {
      this.watcher = fs.watch(dir, (_event, changed) => {
        if (changed && changed !== fileName) {
          return;
        }
        clearTimeout(this.reloadTimer);
        this.reloadTimer = setTimeout(() => {
          this.load().catch((error) => {
            this.lastError = error instanceof Error ? error.message : "failed to reload config";
            this.emit("error", this.lastError);
          });
        }, 300);
      });
    } catch {
      // A missing config directory is acceptable; manual reload still works.
    }
  }

  getConfig() {
    return this.config;
  }

  getSnapshot() {
    return {
      ok: true,
      loaded_at: this.loadedAt,
      last_error: this.lastError,
      config: this.config
    };
  }

  resolveStream(streamKey) {
    const parts = splitStreamKey(streamKey);
    const cfg = this.config;
    const vhost = (cfg.vhosts || []).find((item) => item.name === parts.vhost) || (cfg.vhosts || [])[0] || {};
    const app = (vhost.apps || []).find((item) => item.name === parts.app) || {};
    const stream = (app.streams || []).find((item) => item.name === parts.stream || item.stream_key === streamKey) || {};
    const policy = deepMerge(
      deepMerge(
        deepMerge({
          auth: cfg.auth,
          recording: cfg.recording,
          hls: cfg.hls,
          protocols: cfg.protocols,
          callbacks: cfg.callbacks
        }, vhost.defaults || {}),
        app.defaults || {}
      ),
      stream.defaults || stream
    );
    return {
      stream_key: streamKey,
      vhost: vhost.name || parts.vhost,
      app: parts.app,
      stream: parts.stream,
      enabled: vhost.enabled !== false && app.enabled !== false && stream.enabled !== false,
      policy
    };
  }
}
