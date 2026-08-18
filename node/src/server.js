import express from "express";
import fs from "fs/promises";
import http from "http";
import https from "https";
import path from "path";
import { fileURLToPath } from "url";
import { CallbackManager } from "./callback_manager.js";
import { ConfigManager } from "./config_manager.js";
import { HlsManager } from "./hls_manager.js";
import { RecordingManager } from "./recording_manager.js";
import { RtspCompatManager } from "./rtsp_compat_manager.js";
import { RtspPlaybackServer } from "./rtsp_playback_server.js";
import { RtspPublishServer } from "./rtsp_publish_server.js";
import { RtspProxyManager } from "./rtsp_proxy_manager.js";
import { SrtManager } from "./srt_manager.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const projectRoot = path.resolve(__dirname, "..", "..");
const configManager = new ConfigManager({ projectRoot });
await configManager.load().catch((error) => {
  console.error(`OTTS config load failed, using defaults: ${error instanceof Error ? error.message : "unknown error"}`);
});
configManager.startWatching();
const config = configManager.getConfig();
const callbackManager = new CallbackManager({ configManager });

const app = express();
const port = process.env.PORT || config.ports.nodeHttp || 3000;
const httpsPort = process.env.HTTPS_PORT || config.ports.nodeHttps || 3443;
const apiBase = process.env.OTTS_API_BASE || "http://127.0.0.1:8080";
const webrtcGatewayBase = process.env.OTTS_WEBRTC_GATEWAY_BASE || "http://127.0.0.1:8081";
const tlsKeyPath = process.env.OTTS_TLS_KEY_PATH || "";
const tlsCertPath = process.env.OTTS_TLS_CERT_PATH || "";
const hlsManager = new HlsManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  rootDir: config.hls.rootDir,
  autoStart: config.hls.autoStart,
  segmentSeconds: config.hls.segmentSeconds,
  listSize: config.hls.listSize,
  idleStopMs: Number(config.hls.idleStopSeconds || 15) * 1000,
  cleanupAgeMs: Number(config.hls.cleanupAgeSeconds || 600) * 1000,
  restartBackoffMs: Number(config.hls.restartBackoffSeconds || 5) * 1000,
  playlistStaleMs: Number(config.hls.playlistStaleSeconds || 10) * 1000,
  playlistStartupTimeoutMs: Number(config.hls.playlistStartupTimeoutSeconds || 20) * 1000
});
const recordingManager = new RecordingManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  rootDir: config.recording.rootDir,
  defaultFormat: config.recording.defaultFormat,
  enabled: config.recording.enabled,
  autoRecord: config.recording.autoRecord
});
const rtspProxyManager = new RtspProxyManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  coreApiBase: apiBase
});
const rtspCompatManager = new RtspCompatManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  coreApiBase: apiBase,
  bindHost: process.env.OTTS_RTSP_BIND_HOST || "0.0.0.0",
  publicHost: process.env.OTTS_RTSP_PUBLIC_HOST || "",
  publishPort: Number(process.env.OTTS_RTSP_PUBLISH_PORT || 8554),
  playPort: Number(process.env.OTTS_RTSP_PLAY_PORT || 8556)
});
const rtspPlaybackServer = new RtspPlaybackServer({
  port: Number(process.env.OTTS_RTSP_PLAY_PORT || 8556),
  host: process.env.OTTS_RTSP_BIND_HOST || "0.0.0.0",
  publicHost: process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11",
  apiBase,
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  defaultPlayMode: process.env.OTTS_RTSP_PLAY_MODE || "core-egress-flv"
});
const rtspPublishServer = new RtspPublishServer({
  port: Number(process.env.OTTS_RTSP_PUBLISH_PORT || 8554),
  host: process.env.OTTS_RTSP_BIND_HOST || "0.0.0.0",
  publicHost: process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11",
  apiBase,
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  defaultPublishMode: process.env.OTTS_RTSP_PUBLISH_MODE || "core-direct-flv"
});
const srtManager = new SrtManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  coreApiBase: apiBase,
  publicHost: process.env.OTTS_SRT_PUBLIC_HOST || process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11",
  publishPortBase: Number(process.env.OTTS_SRT_PUBLISH_PORT_BASE || 9000),
  playPortBase: Number(process.env.OTTS_SRT_PLAY_PORT_BASE || 10000),
  defaultPublishMode: process.env.OTTS_SRT_PUBLISH_MODE || "core-direct-flv",
  defaultPlayMode: process.env.OTTS_SRT_PLAY_MODE || "core-egress-flv"
});
const srtBootstrapEnabled = (process.env.OTTS_SRT_BOOTSTRAP_ENABLED || "true") !== "false";
const srtBootstrapStreamKey = process.env.OTTS_SRT_BOOTSTRAP_STREAM_KEY || "live/srt-demo";
const srtBootstrapPlayEnabled = (process.env.OTTS_SRT_BOOTSTRAP_PLAY_ENABLED || "true") !== "false";
const rtspPlaybackEnabled = (process.env.OTTS_RTSP_PLAY_COMPAT_ENABLED || "true") !== "false";
const nativeProtocolOnly = (process.env.OTTS_NATIVE_PROTOCOL_ONLY || "true") !== "false";

function applyRuntimeConfig(snapshot = configManager.getSnapshot()) {
  const next = snapshot.config || configManager.getConfig();
  hlsManager.updateOptions(next.hls || {});
  recordingManager.updateOptions(next.recording || {});
  console.log(`OTTS config applied from ${next.metadata?.configPath || "defaults"} at ${snapshot.loaded_at || new Date().toISOString()}`);
}

configManager.on("reload", (snapshot) => {
  applyRuntimeConfig(snapshot);
});
configManager.on("error", (message) => {
  console.error(`OTTS config reload failed: ${message}`);
});
applyRuntimeConfig();

function buildStreamSummary(streams = []) {
  const summary = {
    total_streams: streams.length,
    by_source_protocol: {},
    by_ingest_origin: {},
    by_manager: {},
    publisher_online_count: 0,
    callback_viewer_count: 0,
    session_viewer_count: 0
  };

  for (const stream of streams) {
    const source = stream.source_protocol || "unknown";
    const origin = stream.ingest_origin || "unknown";
    const manager = stream.managed_by || "core";
    summary.by_source_protocol[source] = (summary.by_source_protocol[source] || 0) + 1;
    summary.by_ingest_origin[origin] = (summary.by_ingest_origin[origin] || 0) + 1;
    summary.by_manager[manager] = (summary.by_manager[manager] || 0) + 1;
    if (stream.has_publisher) {
      summary.publisher_online_count += 1;
    }
    summary.callback_viewer_count += Number(stream.http_flv_viewer_count || 0);
    summary.session_viewer_count += Number(stream.viewer_count || 0);
  }

  return summary;
}

function buildSessionSummary(sessions = [], modes = ["publish", "play"]) {
  const summary = {
    total_count: sessions.length
  };

  for (const mode of modes) {
    summary[`${mode}_desired_count`] = sessions.filter((session) => session[`desired_${mode}`]).length;
    summary[`${mode}_running_count`] = sessions.filter((session) => session[mode]?.running).length;
  }

  return summary;
}

function mergeRtspCompatSessions(desiredSessions = [], publishSessions = []) {
  const merged = new Map();

  for (const session of desiredSessions) {
    merged.set(session.stream_key, {
      ...session,
      native_publish: null
    });
  }

  for (const publishSession of publishSessions) {
    const existing = merged.get(publishSession.stream_key);
    if (existing) {
      existing.native_publish = publishSession;
      if (!existing.publish_url) {
        existing.publish_url = `rtsp://${rtspPublishServer.publicHost}:${rtspPublishServer.port}/${publishSession.path}.sdp`;
      }
    } else {
      merged.set(publishSession.stream_key, {
        stream_key: publishSession.stream_key,
        publish_url: `rtsp://${rtspPublishServer.publicHost}:${rtspPublishServer.port}/${publishSession.path}.sdp`,
        play_url: `rtsp://${rtspPlaybackServer.publicHost}:${rtspPlaybackServer.port}/${publishSession.path}.sdp`,
        target_rtmp_url: `${process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935"}/${publishSession.stream_key}`,
        desired_publish: true,
        desired_play: false,
        publish: null,
        play: null,
        native_publish: publishSession
      });
    }
  }

  return [...merged.values()].sort((left, right) => left.stream_key.localeCompare(right.stream_key));
}

function encodeRtspPath(streamKey) {
  return String(streamKey || "live/stream").replace(/\//g, "__");
}

function nativeRtspUrls(streamKey) {
  const path = encodeRtspPath(streamKey);
  return {
    publish_url: `rtsp://${rtspPublishServer.publicHost}:${rtspPublishServer.port}/${path}.sdp`,
    play_url: `rtsp://${rtspPlaybackServer.publicHost}:${rtspPlaybackServer.port}/${path}.sdp`
  };
}

function nativeSrtUrls() {
  return {
    publish_url: `srt://${process.env.OTTS_SRT_PUBLIC_HOST || process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11"}:${Number(process.env.OTTS_SRT_PUBLISH_PORT_BASE || 9000)}?mode=caller&transtype=live`,
    play_url: `srt://${process.env.OTTS_SRT_PUBLIC_HOST || process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11"}:${Number(process.env.OTTS_SRT_PLAY_PORT_BASE || 10000)}?mode=caller&transtype=live`
  };
}

async function fetchJson(pathname, fallback) {
  try {
    const response = await fetch(`${apiBase}${pathname}`);
    if (!response.ok) {
      throw new Error(`upstream status ${response.status}`);
    }
    return await response.json();
  } catch {
    return fallback;
  }
}

async function getUpstreamStreams() {
  return fetchJson("/api/streams", { streams: [] });
}

async function getProtocolSessions() {
  return fetchJson("/api/sessions", { sessions: [] });
}

async function ensureSrtBootstrap() {
  if (!srtBootstrapEnabled || !srtBootstrapStreamKey) {
    return;
  }

  const current = srtManager.maybeStatusFor(srtBootstrapStreamKey);
  if (!current?.desired_publish || !current?.publish?.running) {
    await srtManager.startPublish(srtBootstrapStreamKey);
  }

  if (!srtBootstrapPlayEnabled) {
    return;
  }

  const streamsState = await getUpstreamStreams();
  const bootstrapStream = (streamsState.streams || []).find((stream) => stream.stream_key === srtBootstrapStreamKey);
  const hasPublisher = Boolean(bootstrapStream?.has_publisher);
  const refreshed = srtManager.maybeStatusFor(srtBootstrapStreamKey);

  if (hasPublisher) {
    if (!refreshed?.desired_play || !refreshed?.play?.running) {
      await srtManager.startPlay(srtBootstrapStreamKey);
    }
    return;
  }

  if (refreshed?.desired_play) {
    await srtManager.stop(srtBootstrapStreamKey, "play");
    await srtManager.startPublish(srtBootstrapStreamKey);
  }
}

async function getSystemStatus() {
  return fetchJson("/api/system/status", {
    ok: false,
    service: "otts-core",
    managed_processes: []
  });
}

const streamEventState = new Map();

function viewerCount(stream = {}) {
  return Number(
    stream.total_viewer_count
      ?? stream.viewer_count
      ?? ((Number(stream.http_flv_viewer_count || 0)) + (Number(stream.webrtc_viewer_count || 0)))
      ?? 0
  );
}

async function emitCallback(eventName, payload) {
  try {
    await callbackManager.emit(eventName, payload);
  } catch (error) {
    console.error(`OTTS callback ${eventName} failed: ${error instanceof Error ? error.message : "unknown error"}`);
  }
}

async function maybeAutoRecord(streamKey, stream) {
  const resolved = configManager.resolveStream(streamKey);
  const recordingPolicy = resolved.policy?.recording || {};
  if (!recordingPolicy.enabled || !recordingPolicy.autoRecord) {
    return;
  }
  const current = recordingManager.getStatus(streamKey);
  if (current.running) {
    return;
  }
  try {
    const status = await recordingManager.start(streamKey, {
      format: recordingPolicy.defaultFormat || "flv",
      streamState: stream
    });
    await emitCallback("on_dvr", {
      stream_key: streamKey,
      source_protocol: stream.source_protocol,
      data: { action: "auto_start", recording: status }
    });
  } catch (error) {
    await emitCallback("on_dvr", {
      stream_key: streamKey,
      source_protocol: stream.source_protocol,
      data: { action: "auto_start_failed", error: error instanceof Error ? error.message : "unknown error" }
    });
  }
}

async function reconcileStreamEvents(streams = []) {
  const liveKeys = new Set();

  for (const stream of streams) {
    if (!stream?.stream_key) {
      continue;
    }
    const streamKey = stream.stream_key;
    liveKeys.add(streamKey);
    const previous = streamEventState.get(streamKey) || {
      hasPublisher: false,
      viewers: 0,
      hlsReady: false
    };
    const hasPublisher = Boolean(stream.has_publisher);
    const viewers = viewerCount(stream);
    const hls = hlsManager.getStatus(streamKey);
    const hlsReady = Boolean(hls.playlist_exists);

    if (hasPublisher && !previous.hasPublisher) {
      await emitCallback("on_publish", {
        stream_key: streamKey,
        source_protocol: stream.source_protocol,
        data: { stream }
      });
      await maybeAutoRecord(streamKey, stream);
    }

    if (!hasPublisher && previous.hasPublisher) {
      await emitCallback("on_unpublish", {
        stream_key: streamKey,
        source_protocol: stream.source_protocol,
        data: { stream }
      });
    }

    if (viewers > 0 && previous.viewers <= 0) {
      await emitCallback("on_play", {
        stream_key: streamKey,
        source_protocol: stream.source_protocol,
        data: { viewers, stream }
      });
    }

    if (viewers <= 0 && previous.viewers > 0) {
      await emitCallback("on_stop", {
        stream_key: streamKey,
        source_protocol: stream.source_protocol,
        data: { viewers, stream }
      });
    }

    if (hlsReady !== previous.hlsReady) {
      await emitCallback("on_hls", {
        stream_key: streamKey,
        source_protocol: stream.source_protocol,
        data: {
          action: hlsReady ? "ready" : "stopped",
          hls
        }
      });
    }

    streamEventState.set(streamKey, {
      hasPublisher,
      viewers,
      hlsReady,
      lastSeenAt: new Date().toISOString()
    });
  }

  for (const [streamKey, previous] of streamEventState.entries()) {
    if (liveKeys.has(streamKey)) {
      continue;
    }
    if (previous.hasPublisher) {
      await emitCallback("on_unpublish", {
        stream_key: streamKey,
        data: { reason: "stream_removed" }
      });
    }
    if (previous.viewers > 0) {
      await emitCallback("on_stop", {
        stream_key: streamKey,
        data: { reason: "stream_removed" }
      });
    }
    streamEventState.delete(streamKey);
  }
}

async function fetchGatewayJson(pathname, fallback) {
  try {
    const nativeState = await getWebRtcNativeStatus();
    if (nativeState.selected_runtime === "native") {
      if (pathname === "/api/streams") {
        return await fetchJson("/api/webrtc/sessions", fallback);
      }
      if (pathname === "/health") {
        return {
          ok: true,
          service: "otts-webrtc-native",
          runtime: nativeState.selected_runtime,
          media_engine_ready: nativeState.media_engine_ready
        };
      }
    }
    const response = await fetch(`${webrtcGatewayBase}${pathname}`);
    if (!response.ok) {
      throw new Error(`gateway status ${response.status}`);
    }
    return await response.json();
  } catch {
    return fallback;
  }
}

async function getWebRtcNativeStatus() {
  return fetchJson("/api/webrtc/native", {
    ok: false,
    selected_runtime: "gateway",
    media_engine_ready: false
  });
}

async function getWebRtcSignalBase() {
  const nativeState = await getWebRtcNativeStatus();
  return nativeState.selected_runtime === "native" ? apiBase : webrtcGatewayBase;
}

function requestTextBody(req) {
  if (typeof req.rawBody === "string") {
    return req.rawBody;
  }
  if (typeof req.body === "string") {
    return req.body;
  }
  if (Buffer.isBuffer(req.body)) {
    return req.body.toString("utf8");
  }
  if (req.body instanceof Uint8Array) {
    return Buffer.from(req.body).toString("utf8");
  }
  return "";
}

async function readRequestTextBody(req) {
  const parsed = requestTextBody(req);
  if (parsed) {
    return parsed;
  }

  const chunks = [];
  for await (const chunk of req) {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  }
  return Buffer.concat(chunks).toString("utf8");
}

async function proxyWebRtcSdp(req, res, targetPath) {
  try {
    const queryIndex = req.originalUrl.indexOf("?");
    const query = queryIndex >= 0 ? req.originalUrl.slice(queryIndex) : "";
    const body = await readRequestTextBody(req);
    const targetBase = await getWebRtcSignalBase();
    const response = await fetch(`${targetBase}${targetPath}${query}`, {
      method: "POST",
      headers: {
        "Content-Type": req.get("content-type") || "application/sdp"
      },
      body
    });

    const text = await response.text();
    const contentType = response.headers.get("content-type") || "application/sdp";
    const location = response.headers.get("location");
    const sessionId = response.headers.get("x-session-id");
    if (location) {
      res.setHeader("Location", location);
    }
    if (sessionId) {
      res.setHeader("X-Session-Id", sessionId);
    }
    res.status(response.status);
    res.type(contentType);
    res.send(text);
  } catch (error) {
    res.status(502).json({
      ok: false,
      error: error instanceof Error ? error.message : "webrtc gateway unavailable"
    });
  }
}

async function proxyWebRtcOffer(req, res, targetPath) {
  try {
    const queryIndex = req.originalUrl.indexOf("?");
    const query = queryIndex >= 0 ? req.originalUrl.slice(queryIndex) : "";
    const targetBase = await getWebRtcSignalBase();
    const response = await fetch(`${targetBase}${targetPath}${query}`, {
      method: "GET"
    });

    const text = await response.text();
    const contentType = response.headers.get("content-type") || "application/sdp";
    const location = response.headers.get("location");
    const sessionId = response.headers.get("x-session-id");
    if (location) {
      res.setHeader("Location", location);
    }
    if (sessionId) {
      res.setHeader("X-Session-Id", sessionId);
    }
    res.status(response.status);
    res.type(contentType);
    res.send(text);
  } catch (error) {
    res.status(502).json({
      ok: false,
      error: error instanceof Error ? error.message : "webrtc native offer unavailable"
    });
  }
}

async function proxyWebRtcSession(req, res) {
  try {
    const targetBase = await getWebRtcSignalBase();
    const body = req.method === "PATCH" || req.method === "POST" ? await readRequestTextBody(req) : undefined;
    const response = await fetch(`${targetBase}${req.originalUrl}`, {
      method: req.method,
      headers: {
        "Content-Type": req.get("content-type") || "text/plain"
      },
      body
    });
    const text = await response.text();
    res.status(response.status);
    const contentType = response.headers.get("content-type");
    if (contentType) {
      res.type(contentType);
    }
    res.send(text);
  } catch (error) {
    res.status(502).json({
      ok: false,
      error: error instanceof Error ? error.message : "webrtc gateway unavailable"
    });
  }
}

function proxyAlias(req, res, targetPath) {
  return proxyWebRtcSdp(req, res, targetPath);
}

function isWebRtcSignalRequest(req) {
  return (
    req.path.startsWith("/whip") ||
    req.path.startsWith("/whep") ||
    req.path.startsWith("/rtc/v1/whip") ||
    req.path.startsWith("/rtc/v1/whep") ||
    (req.path.startsWith("/session/") && (req.method === "PATCH" || req.method === "POST"))
  );
}

app.use((req, _res, next) => {
  if (!isWebRtcSignalRequest(req)) {
    next();
    return;
  }
  const chunks = [];
  req.on("data", (chunk) => {
    chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
  });
  req.on("end", () => {
    req.rawBody = Buffer.concat(chunks).toString("utf8");
    next();
  });
  req.on("error", next);
});
app.use(express.json());
app.use(express.static(path.join(__dirname, "..", "web")));
app.use("/recordings", (req, res, next) => {
  express.static(recordingManager.rootDir)(req, res, next);
});

app.use("/hls", (req, res, next) => {
  express.static(hlsManager.rootDir, {
  fallthrough: true,
  setHeaders(res, filePath) {
    if (filePath.endsWith(".m3u8")) {
      res.setHeader("Content-Type", "application/vnd.apple.mpegurl");
      res.setHeader("Cache-Control", "no-cache");
    } else if (filePath.endsWith(".ts")) {
      res.setHeader("Content-Type", "video/mp2t");
      res.setHeader("Cache-Control", "no-cache");
    }
  }
  })(req, res, next);
});

app.get("/api/config", (_req, res) => {
  res.json(configManager.getSnapshot());
});

app.post("/api/config/reload", async (_req, res) => {
  try {
    const loaded = await configManager.load();
    res.json({ ok: true, loaded_at: configManager.loadedAt, config: loaded });
  } catch (error) {
    res.status(400).json({
      ok: false,
      error: error instanceof Error ? error.message : "failed to reload config"
    });
  }
});

app.get("/api/config/stream", (req, res) => {
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  res.json({ ok: true, stream: configManager.resolveStream(streamKey) });
});

app.get("/api/callbacks/events", (req, res) => {
  res.json({ ok: true, events: callbackManager.listEvents(req.query.limit) });
});

app.get("/api/callbacks/config", (_req, res) => {
  res.json({ ok: true, callbacks: configManager.getConfig().callbacks });
});

app.get("/api/health", async (_req, res) => {
  const health = await fetchJson("/api/health", {
    ok: false,
    service: "otts",
    protocol: "rtmp",
    http_api_port: 8080
  });
  const systemStatus = await getSystemStatus();
  const streams = await getUpstreamStreams();
  await hlsManager.syncStreams(streams.streams || []);
  await reconcileStreamEvents(streams.streams || []);
  const streamList = streams.streams || [];
  const streamSummary = buildStreamSummary(streamList);
  const totalViewers = streamList.reduce(
    (sum, stream) => sum + (stream.total_viewer_count || stream.viewer_count || 0),
    0
  );
  const managedProcesses = systemStatus.managed_processes || [];
  const managedOnline = managedProcesses.filter((process) => process.running).length;
  const rtspPublishSessions = rtspPublishServer.sessionSummaries();
  const rtspCompatSessions = nativeProtocolOnly ? [] : rtspCompatManager.listStatuses();
  const protocolSessions = await getProtocolSessions();
  const nativeSrtSessions = (protocolSessions.sessions || [])
    .filter((session) => session.source_protocol === "srt" && String(session.managed_by || "").startsWith("cpp-srt-"));
  const srtSessions = nativeProtocolOnly ? nativeSrtSessions : srtManager.listStatuses();
  res.json({
    ...health,
    streamCount: streamList.length,
    totalViewers,
    apiBase,
    hlsRoot: hlsManager.rootDir,
    hlsAutoStart: hlsManager.autoStart,
    webrtcGatewayBase,
    runtime: systemStatus,
    managedProcessCount: managedProcesses.length,
    managedProcessOnlineCount: managedOnline,
    rtspProxyCount: rtspProxyManager.listStatuses().length,
    rtspCompatCount: rtspCompatSessions.length,
    rtspCompatPublishDesiredCount: rtspCompatSessions.filter((session) => session.desired_publish).length,
    rtspCompatPlayDesiredCount: rtspCompatSessions.filter((session) => session.desired_play).length,
    rtspNativePublishCount: rtspPublishSessions.length,
    rtspNativeRecordingCount: rtspPublishSessions.filter((session) => session.state === "recording").length,
    srtSessionCount: srtSessions.length,
    srtPublishCount: srtSessions.filter((session) => session.desired_publish).length,
    srtPublishRunningCount: srtSessions.filter((session) => session.publish?.running).length,
    srtPlayCount: srtSessions.filter((session) => session.desired_play).length,
    srtPlayRunningCount: srtSessions.filter((session) => session.play?.running).length,
    recordingRunningCount: recordingManager.listStatuses(streamList).filter((item) => item.running).length,
    streamSummary
  });
});

app.get("/api/streams", async (_req, res) => {
  const state = await getUpstreamStreams();
  await hlsManager.syncStreams(state.streams || []);
  await reconcileStreamEvents(state.streams || []);
  const enriched = (state.streams || []).map((stream) => ({
    ...stream,
    hls: hlsManager.getStatus(stream.stream_key),
    recording: recordingManager.getStatus(stream.stream_key)
  }));
  res.json({ streams: enriched });
});

app.get("/api/protocol/sessions", async (_req, res) => {
  const state = await getProtocolSessions();
  res.json(state);
});

app.get("/api/recordings", async (_req, res) => {
  const files = await recordingManager.listFiles();
  res.json({ ok: true, root_dir: recordingManager.rootDir, files });
});

app.get("/api/recordings/status", async (req, res) => {
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  res.json({ ok: true, recording: recordingManager.getStatus(streamKey) });
});

app.post("/api/recordings/start", async (req, res) => {
  const streamKey = String(req.query.stream_key || "");
  const format = String(req.query.format || "flv");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  try {
    const upstream = await getUpstreamStreams();
    const stream = (upstream.streams || []).find((item) => item.stream_key === streamKey);
    if (!stream?.has_publisher) {
      res.status(404).json({ ok: false, error: "stream is not publishing", recording: recordingManager.getStatus(streamKey) });
      return;
    }
    const status = await recordingManager.start(streamKey, { format, streamState: stream });
    await emitCallback("on_dvr", {
      stream_key: streamKey,
      data: { action: "start", format, recording: status }
    });
    res.json({ ok: true, recording: status });
  } catch (error) {
    res.status(400).json({
      ok: false,
      error: error instanceof Error ? error.message : "failed to start recording"
    });
  }
});

app.post("/api/recordings/stop", async (req, res) => {
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  const status = await recordingManager.stop(streamKey);
  await emitCallback("on_dvr", {
    stream_key: streamKey,
    data: { action: "stop", recording: status }
  });
  res.json({ ok: Boolean(status), recording: status });
});

app.get("/api/recordings/log", async (req, res) => {
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  const lines = await recordingManager.readLogTail(streamKey);
  res.json({ ok: true, lines });
});

app.get("/api/webrtc/sessions", async (_req, res) => {
  const state = await fetchGatewayJson("/api/streams", { sessions: [], published_streams: [] });
  res.json(state);
});

app.get("/api/webrtc/health", async (_req, res) => {
  const state = await fetchGatewayJson("/health", {
    ok: false,
    service: "otts-webrtc-gateway"
  });
  res.json(state);
});

app.get("/api/webrtc/native", async (_req, res) => {
  const state = await getWebRtcNativeStatus();
  res.json({
    ok: false,
    mode: "unknown",
    selected_runtime: "unknown",
    compiled_with_dependency: false,
    dependency_ready: false,
    media_engine_ready: false,
    detail: "core API unavailable",
    ...state
  });
});

app.get("/api/webrtc/native/raw", async (_req, res) => {
  const state = await fetchJson("/api/webrtc/native", {
    ok: false,
    mode: "unknown",
    selected_runtime: "unknown",
    compiled_with_dependency: false,
    dependency_ready: false,
    media_engine_ready: false,
    detail: "core API unavailable"
  });
  res.json(state);
});

app.get("/api/system/status", async (_req, res) => {
  const systemStatus = await getSystemStatus();
  res.json(systemStatus);
});

app.get("/api/rtsp/proxies", async (_req, res) => {
  res.json({
    native_only: nativeProtocolOnly,
    disabled: nativeProtocolOnly,
    proxies: nativeProtocolOnly ? [] : rtspProxyManager.listStatuses()
  });
});

app.get("/api/rtsp/compat", async (_req, res) => {
  const desiredSessions = nativeProtocolOnly ? [] : rtspCompatManager.listStatuses();
  const protocolSessions = await getProtocolSessions();
  const nativeSessions = (protocolSessions.sessions || [])
    .filter((session) => session.source_protocol === "rtsp" && String(session.managed_by || "").startsWith("cpp-rtsp-"));
  const publishSessions = nativeProtocolOnly
    ? nativeSessions.filter((session) => session.direction === "publish")
    : rtspPublishServer.sessionSummaries();
  const playbackSessions = nativeProtocolOnly
    ? nativeSessions.filter((session) => session.direction === "play")
    : (rtspPlaybackEnabled ? rtspPlaybackServer.sessionSummaries() : []);
  const mergedSessions = nativeProtocolOnly
    ? publishSessions.map((session) => ({
        stream_key: session.stream_key,
        ...nativeRtspUrls(session.stream_key),
        desired_publish: true,
        desired_play: true,
        publish: null,
        play: null,
        native_publish: session
      }))
    : mergeRtspCompatSessions(desiredSessions, publishSessions);
  res.json({
    native_only: nativeProtocolOnly,
    disabled: nativeProtocolOnly,
    sessions: desiredSessions,
    publish_sessions: publishSessions,
    playback_sessions: playbackSessions,
    native_sessions: nativeSessions,
    merged_sessions: mergedSessions,
    summary: {
      ...buildSessionSummary(desiredSessions),
      desired_count: desiredSessions.length,
      native_publish_count: publishSessions.length,
      native_recording_count: publishSessions.filter((session) => session.state === "recording").length,
      native_play_count: playbackSessions.length
    }
  });
});

app.get("/api/srt/sessions", async (_req, res) => {
  const sessions = nativeProtocolOnly ? [] : srtManager.listStatuses();
  const protocolSessions = await getProtocolSessions();
  const nativeSessions = (protocolSessions.sessions || [])
    .filter((session) => session.source_protocol === "srt" && String(session.managed_by || "").startsWith("cpp-srt-"));
  const nativeUrls = nativeSrtUrls();
  res.json({
    native_only: nativeProtocolOnly,
    disabled: nativeProtocolOnly,
    urls: nativeUrls,
    sessions,
    native_sessions: nativeSessions,
    summary: {
      ...buildSessionSummary(sessions),
      total_count: nativeProtocolOnly ? nativeSessions.length : sessions.length,
      publish_count: sessions.filter((session) => session.desired_publish).length,
      play_count: sessions.filter((session) => session.desired_play).length,
      native_count: nativeSessions.length,
      native_publish_count: nativeSessions.filter((session) => session.direction === "publish").length,
      native_play_count: nativeSessions.filter((session) => session.direction === "play").length
    }
  });
});

app.post("/api/srt/start", async (req, res) => {
  if (nativeProtocolOnly) {
    res.status(410).json({
      ok: false,
      error: "SRT ffmpeg compatibility workers are disabled; use the native C++ SRT publish/play ports directly",
      urls: nativeSrtUrls()
    });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "both");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  try {
    let status = srtManager.statusFor(streamKey);
    if (mode === "both" || mode === "publish") {
      status = await srtManager.startPublish(streamKey);
    }
    if (mode === "both" || mode === "play") {
      status = await srtManager.startPlay(streamKey);
    }
    res.json({ ok: true, session: status });
  } catch (error) {
    res.status(400).json({
      ok: false,
      error: error instanceof Error ? error.message : "failed to start srt session"
    });
  }
});

app.post("/api/srt/stop", async (req, res) => {
  if (nativeProtocolOnly) {
    res.json({ ok: true, disabled: true, message: "SRT native path has no Node worker to stop" });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "both");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  const stopped = await srtManager.stop(streamKey, mode);
  res.json({ ok: stopped, session: srtManager.maybeStatusFor(streamKey) });
});

app.get("/api/srt/log", async (req, res) => {
  if (nativeProtocolOnly) {
    res.json({ ok: true, disabled: true, lines: ["SRT native path is handled by the C++ core; see OTTS runtime log."] });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "publish");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }
  const lines = await srtManager.readLogTail(streamKey, mode);
  res.json({ ok: true, lines });
});

app.post("/api/rtsp/proxies/start", async (req, res) => {
  if (nativeProtocolOnly) {
    res.status(410).json({
      ok: false,
      error: "RTSP pull relay compatibility workers are disabled in native protocol mode"
    });
    return;
  }
  try {
    const status = await rtspProxyManager.start({
      streamKey: String(req.query.stream_key || ""),
      sourceUrl: String(req.query.source_url || ""),
      transport: String(req.query.transport || "tcp")
    });
    res.json({ ok: true, proxy: status });
  } catch (error) {
    res.status(400).json({
      ok: false,
      error: error instanceof Error ? error.message : "failed to start rtsp proxy"
    });
  }
});

app.post("/api/rtsp/proxies/stop", async (req, res) => {
  if (nativeProtocolOnly) {
    res.json({ ok: true, disabled: true, message: "RTSP pull relay compatibility worker is disabled" });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const stopped = await rtspProxyManager.stop(streamKey);
  res.json({ ok: stopped, proxy: rtspProxyManager.getStatus(streamKey) });
});

app.get("/api/rtsp/proxies/log", async (req, res) => {
  if (nativeProtocolOnly) {
    res.json({ ok: true, disabled: true, lines: ["RTSP pull relay compatibility worker is disabled in native protocol mode."] });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const lines = await rtspProxyManager.readLogTail(streamKey);
  res.json({ ok: true, lines });
});

app.post("/api/rtsp/compat/start", async (req, res) => {
  if (nativeProtocolOnly) {
    const streamKey = String(req.query.stream_key || "live/rtsp-demo");
    res.status(410).json({
      ok: false,
      error: "RTSP ffmpeg compatibility workers are disabled; use native RTSP publish/play endpoints directly",
      ...nativeRtspUrls(streamKey)
    });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "both");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  try {
    let status = rtspCompatManager.statusFor(streamKey);
    if (mode === "both" || mode === "publish") {
      status = await rtspCompatManager.startPublish(streamKey);
    }
    if (mode === "both" || mode === "play") {
      status = await rtspCompatManager.startPlay(streamKey);
    }
    res.json({ ok: true, session: status });
  } catch (error) {
    res.status(400).json({
      ok: false,
      error: error instanceof Error ? error.message : "failed to start rtsp compat"
    });
  }
});

app.post("/api/rtsp/compat/stop", async (req, res) => {
  if (nativeProtocolOnly) {
    res.json({ ok: true, disabled: true, message: "RTSP native path has no compatibility worker to stop" });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "both");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const session = await rtspCompatManager.stop(streamKey, mode);
  res.json({ ok: true, session });
});

app.get("/api/rtsp/compat/log", async (req, res) => {
  if (nativeProtocolOnly) {
    res.json({ ok: true, disabled: true, lines: ["RTSP native path is handled by the C++ core/RTSP ingress; see OTTS runtime log."] });
    return;
  }
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "publish");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const lines = await rtspCompatManager.readLogTail(streamKey, mode);
  res.json({ ok: true, lines });
});

app.get("/api/streams/hls/status", async (req, res) => {
  const streamKey = req.query.stream_key;
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  res.json({ ok: true, hls: hlsManager.getStatus(String(streamKey)) });
});

app.post("/whip", async (req, res) => proxyAlias(req, res, "/whip"));
app.post("/whip/v1", async (req, res) => proxyAlias(req, res, "/whip/v1"));
app.post("/rtc/v1/whip", async (req, res) => proxyAlias(req, res, "/whip/v1"));
app.post("/rtc/v1/whip/", async (req, res) => proxyAlias(req, res, "/whip/v1"));
app.post("/whep", async (req, res) => proxyAlias(req, res, "/whep"));
app.post("/whep/v1", async (req, res) => proxyAlias(req, res, "/whep/v1"));
app.post("/rtc/v1/whep", async (req, res) => proxyAlias(req, res, "/whep/v1"));
app.post("/rtc/v1/whep/", async (req, res) => proxyAlias(req, res, "/whep/v1"));
app.get("/whep/offer/v1", async (req, res) => proxyWebRtcOffer(req, res, "/whep/offer/v1"));
app.get("/rtc/v1/whep/offer", async (req, res) => proxyWebRtcOffer(req, res, "/whep/offer/v1"));
app.patch("/session/:sessionId", async (req, res) => proxyWebRtcSession(req, res));
app.post("/session/:sessionId/answer", async (req, res) => proxyWebRtcSession(req, res));
app.delete("/session/:sessionId", async (req, res) => proxyWebRtcSession(req, res));

app.post("/api/streams/disconnect", async (req, res) => {
  const streamKey = req.query.stream_key;
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  try {
    const response = await fetch(
      `${apiBase}/api/streams/disconnect?stream_key=${encodeURIComponent(streamKey)}`,
      { method: "POST" }
    );
    const payload = await response.json();
    res.status(response.ok ? 200 : 502).json(payload);
  } catch {
    res.status(502).json({ ok: false, error: "upstream unavailable" });
  }
});

app.post("/api/streams/hls/start", async (req, res) => {
  const streamKey = req.query.stream_key;
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const upstream = await getUpstreamStreams();
  const stream = (upstream.streams || []).find((item) => item.stream_key === String(streamKey));
  if (!stream?.has_publisher) {
    res.status(404).json({ ok: false, error: "stream is not publishing", hls: hlsManager.getStatus(String(streamKey)) });
    return;
  }

  const status = await hlsManager.ensureRunning(String(streamKey), stream);
  if (status.start_blocked) {
    res.status(409).json({ ok: false, error: status.last_error || "HLS start blocked", hls: status });
    return;
  }
  const ready = await hlsManager.waitForPlaylist(String(streamKey), hlsManager.playlistStartupTimeoutMs);
  await emitCallback("on_hls", {
    stream_key: String(streamKey),
    data: { action: "start", ready, hls: hlsManager.getStatus(String(streamKey)) }
  });
  res.json({ ok: ready, hls: hlsManager.getStatus(String(streamKey)) });
});

app.post("/api/streams/hls/stop", async (req, res) => {
  const streamKey = req.query.stream_key;
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const stopped = await hlsManager.stop(String(streamKey));
  await emitCallback("on_hls", {
    stream_key: String(streamKey),
    data: { action: "stop", hls: hlsManager.getStatus(String(streamKey)) }
  });
  res.json({ ok: stopped, hls: hlsManager.getStatus(String(streamKey)) });
});

app.post("/api/streams/hls/cleanup", async (_req, res) => {
  const removed = await hlsManager.cleanupStaleOutputsWithReport(false);
  await emitCallback("on_hls", {
    stream_key: "",
    data: { action: "cleanup", removed }
  });
  res.json({ ok: true, removed });
});

app.get("/api/streams/hls/log", async (req, res) => {
  const streamKey = req.query.stream_key;
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const lines = await hlsManager.readLogTail(String(streamKey));
  res.json({ ok: true, lines });
});

app.get("/api/debug/runtime-log", async (_req, res) => {
  try {
    const content = await fs.readFile("/tmp/otts_runtime.log", "utf8");
    const lines = content.split(/\r?\n/).filter(Boolean).slice(-120);
    res.json({ ok: true, lines });
  } catch {
    res.json({ ok: false, lines: [] });
  }
});

http.createServer(app).listen(port, () => {
  console.log(`OTTS control plane listening on http://0.0.0.0:${port}`);
});

if (!nativeProtocolOnly) {
  rtspPublishServer.start();
} else {
  console.log("OTTS RTSP compatibility publish disabled; C++ native RTSP publish owns the publish port.");
}
if (!nativeProtocolOnly && rtspPlaybackEnabled) {
  rtspPlaybackServer.start();
} else {
  console.log("OTTS RTSP compatibility playback disabled; C++ native RTSP play owns the play port.");
}

if (tlsKeyPath && tlsCertPath) {
  try {
    const [key, cert] = await Promise.all([
      fs.readFile(tlsKeyPath),
      fs.readFile(tlsCertPath)
    ]);
    https.createServer({ key, cert }, app).listen(httpsPort, () => {
      console.log(`OTTS control plane listening on https://0.0.0.0:${httpsPort}`);
    });
  } catch (error) {
    console.error(`OTTS HTTPS disabled: ${error instanceof Error ? error.message : "unknown TLS error"}`);
  }
}

setInterval(async () => {
  const state = await getUpstreamStreams();
  await hlsManager.syncStreams(state.streams || []);
  await reconcileStreamEvents(state.streams || []);
}, 5000);

if (!nativeProtocolOnly && srtBootstrapEnabled && srtBootstrapStreamKey) {
  setTimeout(async () => {
    try {
      await ensureSrtBootstrap();
      console.log(`OTTS SRT bootstrap active for ${srtBootstrapStreamKey}`);
    } catch (error) {
      console.error(`OTTS SRT bootstrap failed: ${error instanceof Error ? error.message : "unknown error"}`);
    }
  }, 1000);

  setInterval(async () => {
    try {
      await ensureSrtBootstrap();
    } catch (error) {
      console.error(`OTTS SRT bootstrap reconcile failed: ${error instanceof Error ? error.message : "unknown error"}`);
    }
  }, 5000);
}
