import express from "express";
import fs from "fs/promises";
import http from "http";
import https from "https";
import path from "path";
import { fileURLToPath } from "url";
import { HlsManager } from "./hls_manager.js";
import { RtspCompatManager } from "./rtsp_compat_manager.js";
import { RtspPlaybackServer } from "./rtsp_playback_server.js";
import { RtspPublishServer } from "./rtsp_publish_server.js";
import { RtspProxyManager } from "./rtsp_proxy_manager.js";
import { SrtManager } from "./srt_manager.js";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
const port = process.env.PORT || 3000;
const httpsPort = process.env.HTTPS_PORT || 3443;
const apiBase = process.env.OTTS_API_BASE || "http://127.0.0.1:8080";
const webrtcGatewayBase = process.env.OTTS_WEBRTC_GATEWAY_BASE || "http://127.0.0.1:8081";
const tlsKeyPath = process.env.OTTS_TLS_KEY_PATH || "";
const tlsCertPath = process.env.OTTS_TLS_CERT_PATH || "";
const hlsManager = new HlsManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935"
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
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg"
});
const rtspPublishServer = new RtspPublishServer({
  port: Number(process.env.OTTS_RTSP_PUBLISH_PORT || 8554),
  host: process.env.OTTS_RTSP_BIND_HOST || "0.0.0.0",
  publicHost: process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11",
  apiBase,
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg"
});
const srtManager = new SrtManager({
  ffmpegBin: process.env.OTTS_FFMPEG_BIN || "ffmpeg",
  rtmpBase: process.env.OTTS_RTMP_BASE || "rtmp://127.0.0.1:1935",
  coreApiBase: apiBase,
  publicHost: process.env.OTTS_SRT_PUBLIC_HOST || process.env.OTTS_RTSP_PUBLIC_HOST || "192.168.40.11",
  publishPortBase: Number(process.env.OTTS_SRT_PUBLISH_PORT_BASE || 9000),
  playPortBase: Number(process.env.OTTS_SRT_PLAY_PORT_BASE || 10000)
});

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

async function getSystemStatus() {
  return fetchJson("/api/system/status", {
    ok: false,
    service: "otts-core",
    managed_processes: []
  });
}

async function fetchGatewayJson(pathname, fallback) {
  try {
    const response = await fetch(`${webrtcGatewayBase}${pathname}`);
    if (!response.ok) {
      throw new Error(`gateway status ${response.status}`);
    }
    return await response.json();
  } catch {
    return fallback;
  }
}

async function proxyWebRtcSdp(req, res, targetPath) {
  try {
    const queryIndex = req.originalUrl.indexOf("?");
    const query = queryIndex >= 0 ? req.originalUrl.slice(queryIndex) : "";
    const response = await fetch(`${webrtcGatewayBase}${targetPath}${query}`, {
      method: "POST",
      headers: {
        "Content-Type": req.get("content-type") || "application/sdp"
      },
      body: typeof req.body === "string" ? req.body : ""
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

function proxyAlias(req, res, targetPath) {
  return proxyWebRtcSdp(req, res, targetPath);
}

app.use(express.text({ type: ["application/sdp", "text/plain"], limit: "10mb" }));
app.use(express.json());
app.use(express.static(path.join(__dirname, "..", "web")));
app.use("/hls", express.static(hlsManager.rootDir, {
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
}));

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
  const streamList = streams.streams || [];
  const streamSummary = buildStreamSummary(streamList);
  const totalViewers = streamList.reduce(
    (sum, stream) => sum + (stream.total_viewer_count || stream.viewer_count || 0),
    0
  );
  const managedProcesses = systemStatus.managed_processes || [];
  const managedOnline = managedProcesses.filter((process) => process.running).length;
  const rtspPublishSessions = rtspPublishServer.sessionSummaries();
  const rtspCompatSessions = rtspCompatManager.listStatuses();
  const srtSessions = srtManager.listStatuses();
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
    streamSummary
  });
});

app.get("/api/streams", async (_req, res) => {
  const state = await getUpstreamStreams();
  await hlsManager.syncStreams(state.streams || []);
  const enriched = (state.streams || []).map((stream) => ({
    ...stream,
    hls: hlsManager.getStatus(stream.stream_key)
  }));
  res.json({ streams: enriched });
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

app.get("/api/system/status", async (_req, res) => {
  const systemStatus = await getSystemStatus();
  res.json(systemStatus);
});

app.get("/api/rtsp/proxies", async (_req, res) => {
  res.json({ proxies: rtspProxyManager.listStatuses() });
});

app.get("/api/rtsp/compat", async (_req, res) => {
  const desiredSessions = rtspCompatManager.listStatuses();
  const publishSessions = rtspPublishServer.sessionSummaries();
  res.json({
    sessions: desiredSessions,
    publish_sessions: publishSessions,
    merged_sessions: mergeRtspCompatSessions(desiredSessions, publishSessions),
    summary: {
      ...buildSessionSummary(desiredSessions),
      desired_count: desiredSessions.length,
      native_publish_count: publishSessions.length,
      native_recording_count: publishSessions.filter((session) => session.state === "recording").length
    }
  });
});

app.get("/api/srt/sessions", async (_req, res) => {
  const sessions = srtManager.listStatuses();
  res.json({
    sessions,
    summary: {
      ...buildSessionSummary(sessions),
      total_count: sessions.length,
      publish_count: sessions.filter((session) => session.desired_publish).length,
      play_count: sessions.filter((session) => session.desired_play).length
    }
  });
});

app.post("/api/srt/start", async (req, res) => {
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
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const stopped = await rtspProxyManager.stop(streamKey);
  res.json({ ok: stopped, proxy: rtspProxyManager.getStatus(streamKey) });
});

app.get("/api/rtsp/proxies/log", async (req, res) => {
  const streamKey = String(req.query.stream_key || "");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const lines = await rtspProxyManager.readLogTail(streamKey);
  res.json({ ok: true, lines });
});

app.post("/api/rtsp/compat/start", async (req, res) => {
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
  const streamKey = String(req.query.stream_key || "");
  const mode = String(req.query.mode || "both");
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const session = await rtspCompatManager.stop(streamKey, mode);
  res.json({ ok: true, session: rtspCompatManager.maybeStatusFor(streamKey) || session });
});

app.get("/api/rtsp/compat/log", async (req, res) => {
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

  const status = await hlsManager.ensureRunning(String(streamKey));
  const ready = await hlsManager.waitForPlaylist(String(streamKey), 8000);
  res.json({ ok: ready, hls: hlsManager.getStatus(String(streamKey)) });
});

app.post("/api/streams/hls/stop", async (req, res) => {
  const streamKey = req.query.stream_key;
  if (!streamKey) {
    res.status(400).json({ ok: false, error: "missing stream_key" });
    return;
  }

  const stopped = await hlsManager.stop(String(streamKey));
  res.json({ ok: stopped, hls: hlsManager.getStatus(String(streamKey)) });
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

rtspPublishServer.start();
rtspPlaybackServer.start();

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
}, 5000);
