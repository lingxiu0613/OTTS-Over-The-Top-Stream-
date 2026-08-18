import crypto from "crypto";
import dgram from "dgram";
import fs from "fs/promises";
import http from "http";
import https from "https";
import net from "net";
import { spawn } from "child_process";
import { buildRtmpUrl, isStreamTokenAuthorizedFromUri } from "./rtmp_url.js";
import { URLSearchParams } from "url";

function normalizeRtspMount(value) {
  let mount = String(value || "/").trim();
  const queryIndex = mount.indexOf("?");
  if (queryIndex >= 0) {
    mount = mount.slice(0, queryIndex);
  }
  mount = mount.replace(/^[a-z]+:\/\/[^/]+/i, "");
  if (!mount.startsWith("/")) {
    mount = `/${mount}`;
  }
  if (mount.endsWith("/")) {
    mount = mount.slice(0, -1);
  }
  if (mount.endsWith(".sdp")) {
    mount = mount.slice(0, -4);
  }
  return mount.replace(/^\/+/, "");
}

function streamKeyFromUri(uri) {
  const mount = normalizeRtspMount(uri);
  return mount.replaceAll("__", "/");
}

function buildRtspResponse(status, headers = {}, body = "") {
  const lines = [`RTSP/1.0 ${status}`];
  for (const [key, value] of Object.entries(headers)) {
    lines.push(`${key}: ${value}`);
  }
  lines.push("");
  lines.push(body);
  return lines.join("\r\n");
}

function parseSdp(body) {
  const lines = String(body || "")
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean);
  let spropParameterSets = [];
  let trackControl = "streamid=0";
  let videoCodec = "h264";
  for (const line of lines) {
    if (line.startsWith("a=fmtp:") && line.includes("sprop-parameter-sets=")) {
      const match = line.match(/sprop-parameter-sets=([^;]+)/i);
      if (match) {
        spropParameterSets = match[1].split(",").map((item) => item.trim()).filter(Boolean);
      }
      if (line.toLowerCase().includes("h265")) {
        videoCodec = "h265";
      }
    } else if (line.startsWith("a=control:")) {
      const control = line.slice("a=control:".length).trim();
      if (control && control !== "*") {
        trackControl = control;
      }
    } else if (line.startsWith("m=video")) {
      videoCodec = "h264";
    }
  }
  return {
    spropParameterSets,
    trackControl,
    videoCodec
  };
}

function parseTransportPorts(transport) {
  const match = String(transport || "").match(/client_port=(\d+)-(\d+)/i);
  if (!match) {
    return null;
  }
  return {
    rtp: Number(match[1]),
    rtcp: Number(match[2])
  };
}

async function bindUdpSocket(host) {
  const socket = dgram.createSocket("udp4");
  await new Promise((resolve, reject) => {
    socket.once("error", reject);
    socket.bind(0, host, () => {
      socket.removeListener("error", reject);
      resolve();
    });
  });
  return socket;
}

async function bindUdpSocketAt(host, port) {
  const socket = dgram.createSocket("udp4");
  await new Promise((resolve, reject) => {
    socket.once("error", reject);
    socket.bind(port, host, () => {
      socket.removeListener("error", reject);
      resolve();
    });
  });
  return socket;
}

async function bindUdpSocketPair(host) {
  for (let attempt = 0; attempt < 32; attempt += 1) {
    const rtpSocket = await bindUdpSocket(host);
    const rtpPort = rtpSocket.address().port;
    if ((rtpPort % 2) !== 0) {
      rtpSocket.close();
      continue;
    }
    try {
      const rtcpSocket = await bindUdpSocketAt(host, rtpPort + 1);
      return {
        rtpSocket,
        rtcpSocket,
        rtpPort,
        rtcpPort: rtpPort + 1
      };
    } catch {
      rtpSocket.close();
    }
  }
  throw new Error("failed to allocate contiguous UDP port pair");
}

export class RtspPublishServer {
  constructor(options = {}) {
    this.port = options.port || 8554;
    this.host = options.host || "0.0.0.0";
    this.publicHost = options.publicHost || "127.0.0.1";
    this.apiBase = options.apiBase || "http://127.0.0.1:8080";
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.ffmpegBin = options.ffmpegBin || "ffmpeg";
    this.defaultPublishMode = options.defaultPublishMode || "core-direct-flv";
    this.server = null;
    this.connections = new Set();
    this.activeSessions = new Map();
  }

  start() {
    if (this.server) {
      return;
    }
    this.server = net.createServer((socket) => this.handleConnection(socket));
    this.server.listen(this.port, this.host);
  }

  stop() {
    if (!this.server) {
      return;
    }
    for (const connection of this.connections) {
      this.cleanupConnection(connection);
      connection.socket.destroy();
    }
    this.connections.clear();
    this.server.close();
    this.server = null;
  }

  sessionSummaries() {
    return [...this.activeSessions.values()].map((session) => ({
      stream_key: session.streamKey,
      path: session.streamPath,
      state: session.state,
      client_address: session.remoteAddress,
      client_rtp_port: session.clientPorts?.rtp || 0,
      server_rtp_port: session.serverPorts?.rtp || 0,
      packets_received: session.packetCount,
      bytes_received: session.byteCount,
      ffmpeg_pid: session.ffmpeg?.pid || null,
      publish_mode: session.publishMode || this.defaultPublishMode,
      stdout_chunks: session.stdoutChunks || 0,
      stdout_bytes: session.stdoutBytes || 0,
      parsed_tags: session.parsedTags || 0,
      media_packets: session.mediaPackets || 0,
      media_bytes: session.mediaBytes || 0,
      last_media_timestamp: session.lastMediaTimestamp || 0,
      announced_at: session.announcedAt,
      record_started_at: session.recordStartedAt || null
    }));
  }

  sessionKey(connection) {
    return `rtsp-native-publish:${connection.sessionId}`;
  }

  publicUrl(connection) {
    return `rtsp://${this.publicHost}:${this.port}/${connection.streamPath}.sdp`;
  }

  bindUrl(connection) {
    return `rtsp://0.0.0.0:${this.port}/${connection.streamPath}.sdp`;
  }

  async syncCoreSession(connection, stateOverride = "") {
    if (!connection.streamKey || !connection.streamPath) {
      return;
    }
    const publishMode = connection.publishMode || this.defaultPublishMode;
    const isCoreDirect = publishMode === "core-direct-flv";
    const params = new URLSearchParams({
      session_key: this.sessionKey(connection),
      stream_key: connection.streamKey,
      source_protocol: "rtsp",
      direction: "publish",
      managed_by: "node-rtsp-native-publish",
      state: stateOverride || connection.state || "connected",
      public_url: this.publicUrl(connection),
      bind_url: this.bindUrl(connection),
      target_url: isCoreDirect ? `${this.apiBase}/api/internal/media/publish/flv-stream` : `${this.rtmpBase}/${connection.streamKey}`,
      transport: "rtsp/rtp-udp",
      media_path: isCoreDirect ? "rtsp-ffmpeg-flv-stream-core-parser" : "rtsp-native-control+ffmpeg-rtmp-bridge",
      native_stage: isCoreDirect ? "core-direct" : "native-control",
      codec_hint: "h264",
      pid: String(connection.ffmpeg?.pid || 0),
      started_at_epoch_ms: connection.recordStartedAt ? String(Date.parse(connection.recordStartedAt)) : "0",
      last_stopped_at_epoch_ms: stateOverride === "closed" ? String(Date.now()) : "0",
      restart_count: "0",
      last_exit_code: "0",
      last_error: connection.lastError || ""
    });
    try {
      await fetch(`${this.apiBase}/api/internal/sessions/upsert?${params.toString()}`, { method: "POST" });
    } catch {
      // best effort
    }
  }

  async removeCoreSession(connection) {
    const params = new URLSearchParams({
      session_key: this.sessionKey(connection)
    });
    try {
      await fetch(`${this.apiBase}/api/internal/sessions/remove?${params.toString()}`, { method: "POST" });
    } catch {
      // best effort
    }
  }

  handleConnection(socket) {
    const connection = {
      socket,
      buffer: Buffer.alloc(0),
      sessionId: crypto.randomBytes(8).toString("hex"),
      streamKey: "",
      streamPath: "",
      sdp: null,
      clientPorts: null,
      serverPorts: null,
      portHoldRtp: null,
      portHoldRtcp: null,
      sdpPath: "",
      ffmpeg: null,
      remoteAddress: (socket.remoteAddress || "").replace("::ffff:", ""),
      packetCount: 0,
      byteCount: 0,
      state: "connected",
      announcedAt: null,
      recordStartedAt: null,
      lastError: "",
      authorized: false,
      publishMode: this.defaultPublishMode,
      stdoutChunks: 0,
      stdoutBytes: 0,
      parsedTags: 0,
      mediaPackets: 0,
      mediaBytes: 0,
      lastMediaTimestamp: 0
    };
    this.connections.add(connection);

    socket.on("data", (chunk) => {
      connection.buffer = Buffer.concat([connection.buffer, chunk]);
      this.processBufferedRequests(connection).catch((error) => {
        connection.lastError = error instanceof Error ? error.message : String(error);
        this.cleanupConnection(connection);
        socket.destroy();
      });
    });

    socket.on("close", () => {
      this.cleanupConnection(connection);
      this.connections.delete(connection);
    });

    socket.on("error", () => {
      this.cleanupConnection(connection);
      this.connections.delete(connection);
    });
  }

  async processBufferedRequests(connection) {
    while (true) {
      const headerEnd = connection.buffer.indexOf(Buffer.from("\r\n\r\n"));
      if (headerEnd < 0) {
        return;
      }
      const headerText = connection.buffer.slice(0, headerEnd).toString("utf8");
      const lines = headerText.split("\r\n");
      const contentLengthLine = lines.find((line) => line.toLowerCase().startsWith("content-length:"));
      const contentLength = contentLengthLine ? Number(contentLengthLine.split(":")[1].trim()) || 0 : 0;
      const totalLength = headerEnd + 4 + contentLength;
      if (connection.buffer.length < totalLength) {
        return;
      }
      const raw = connection.buffer.slice(0, totalLength).toString("utf8");
      connection.buffer = connection.buffer.slice(totalLength);
      await this.handleRequest(connection, raw);
    }
  }

  parseRequest(raw) {
    const [head, body = ""] = raw.split("\r\n\r\n");
    const lines = head.split("\r\n").filter(Boolean);
    const [requestLine, ...headerLines] = lines;
    const [method, uri] = requestLine.split(" ");
    const headers = {};
    for (const line of headerLines) {
      const index = line.indexOf(":");
      if (index < 0) {
        continue;
      }
      headers[line.slice(0, index).trim().toLowerCase()] = line.slice(index + 1).trim();
    }
    return { method, uri, headers, body };
  }

  send(connection, status, headers = {}, body = "") {
    connection.socket.write(buildRtspResponse(status, headers, body));
  }

  async upsertStream(streamKey, hasPublisher) {
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "rtsp",
      managed_by: "node-rtsp-native",
      has_publisher: hasPublisher ? "true" : "false",
      video_codec: "h264"
    });
    const path = hasPublisher ? "/api/internal/streams/upsert" : "/api/internal/streams/remove";
    try {
      await fetch(`${this.apiBase}${path}?${params.toString()}`, { method: "POST" });
    } catch {
      // best effort
    }
  }

  async publishMediaToCore(streamKey, message) {
    const params = new URLSearchParams({
      stream_key: streamKey,
      source_protocol: "rtsp",
      managed_by: "node-rtsp-native-publish",
      message_type: String(message.typeId),
      timestamp: String(message.timestamp || 0),
      message_stream_id: String(message.messageStreamId || 1),
      payload_encoding: "raw"
    });
    const payload = Buffer.from(message.payload || []);
    if (!payload.length) {
      return;
    }
    const response = await fetch(`${this.apiBase}/api/internal/media/publish?${params.toString()}`, {
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
      source_protocol: "rtsp",
      managed_by: "node-rtsp-native-publish"
    });
    const response = await fetch(`${this.apiBase}/api/internal/media/publish/flv-chunk?${params.toString()}`, {
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
      source_protocol: "rtsp",
      managed_by: "node-rtsp-native-publish"
    });
    const target = new URL(`${this.apiBase}/api/internal/media/publish/flv-stream?${params.toString()}`);
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

  async ensureReceiverProcess(connection) {
    if (!connection.sdp || !connection.serverPorts) {
      throw new Error("ANNOUNCE and SETUP are required before starting receiver");
    }
    if (connection.ffmpeg && !connection.ffmpeg.killed) {
      return;
    }
    const sdpBody = [
      "v=0",
      `o=- 0 0 IN IP4 ${this.publicHost}`,
      "s=OTTS RTSP Ingest",
      `c=IN IP4 ${this.publicHost}`,
      "t=0 0",
      `m=video ${connection.serverPorts.rtp} RTP/AVP 96`,
      "a=rtpmap:96 H264/90000",
      `a=fmtp:96 packetization-mode=1; sprop-parameter-sets=${connection.sdp.spropParameterSets.join(",")}`,
      ""
    ].join("\r\n");
    connection.sdpPath = `/tmp/otts_rtsp_publish_${connection.sessionId}.sdp`;
    await fs.writeFile(connection.sdpPath, sdpBody, "utf8");

    if (connection.portHoldRtp) {
      connection.portHoldRtp.close();
      connection.portHoldRtp = null;
    }
    if (connection.portHoldRtcp) {
      connection.portHoldRtcp.close();
      connection.portHoldRtcp = null;
    }

    connection.publishMode = connection.publishMode || this.defaultPublishMode;
    connection.stdoutChunks = 0;
    connection.stdoutBytes = 0;
    connection.parsedTags = 0;
    connection.mediaPackets = 0;
    connection.mediaBytes = 0;
    connection.lastMediaTimestamp = 0;

    const publishMode = connection.publishMode;
    const isCoreDirect = publishMode === "core-direct-flv";
    let coreStream = null;
    let pendingStdout = [];
    const attachCoreStreamHandlers = () => {
      if (!coreStream) {
        return;
      }
      coreStream.done
        .then((result) => {
          connection.parsedTags += Number(result.parsed_tags || 0);
          connection.mediaPackets += Number(result.media_packets || 0);
          connection.mediaBytes += Number(result.media_bytes || 0);
          if (result.last_media_timestamp !== undefined) {
            connection.lastMediaTimestamp = Number(result.last_media_timestamp || 0);
          }
        })
        .catch((error) => {
          connection.lastError = error instanceof Error ? error.message : String(error);
          if (connection.ffmpeg && !connection.ffmpeg.killed) {
            connection.ffmpeg.kill("SIGTERM");
          }
        });
    };
    const writeToCoreStream = (chunk) => {
      if (!coreStream) {
        pendingStdout.push(Buffer.from(chunk));
        return;
      }
      const canContinue = coreStream.request.write(chunk);
      if (!canContinue && connection.ffmpeg?.stdout) {
        connection.ffmpeg.stdout.pause();
        coreStream.request.once("drain", () => connection.ffmpeg?.stdout?.resume());
      }
    };
    if (isCoreDirect) {
      coreStream = this.openFlvStreamToCore(connection.streamKey);
      attachCoreStreamHandlers();
    }
    const outputTarget = isCoreDirect ? "pipe:1" : buildRtmpUrl(this.rtmpBase, connection.streamKey, "publish");
    const ffmpegArgs = [
      "-hide_banner",
      "-loglevel",
      "info",
      "-protocol_whitelist",
      "file,udp,rtp",
      "-fflags",
      "+genpts",
      "-analyzeduration",
      "10000000",
      "-probesize",
      "5000000",
      "-i",
      connection.sdpPath,
      "-an",
      "-c:v",
      "libx264",
      "-preset",
      "veryfast",
      "-tune",
      "zerolatency",
      "-pix_fmt",
      "yuv420p",
      "-g",
      "50",
      "-bf",
      "0",
      "-flvflags",
      "no_duration_filesize",
      "-f",
      "flv",
      outputTarget
    ];
    connection.ffmpeg = spawn(this.ffmpegBin, ffmpegArgs, {
      stdio: ["ignore", isCoreDirect ? "pipe" : "ignore", "pipe"]
    });
    let startupRejected = false;
    let startupTimer = null;
    let startupResolve = null;
    let startupReject = null;
    const startupPromise = new Promise((resolve, reject) => {
      startupResolve = resolve;
      startupReject = reject;
    });
    startupTimer = setTimeout(() => {
      startupResolve();
    }, 400);
    connection.ffmpeg.once("spawn", () => {
      connection.state = "receiver-starting";
      if (isCoreDirect && pendingStdout.length) {
        for (const pendingChunk of pendingStdout) {
          writeToCoreStream(pendingChunk);
        }
        pendingStdout = [];
      }
      this.syncCoreSession(connection).catch(() => {
        // best effort
      });
    });
    connection.ffmpeg.once("error", (error) => {
      if (startupTimer) {
        clearTimeout(startupTimer);
      }
      startupRejected = true;
      startupReject(error);
    });
    connection.ffmpeg.stdout?.on("data", (chunk) => {
      connection.stdoutChunks += 1;
      connection.stdoutBytes += chunk.length;
      if (!isCoreDirect) {
        return;
      }
      writeToCoreStream(chunk);
    });
    connection.ffmpeg.stderr.on("data", (chunk) => {
      const text = chunk.toString("utf8").trim();
      if (text) {
        console.error(`[rtsp_publish] ${connection.streamKey} ${text}`);
      }
    });
    connection.ffmpeg.on("exit", async () => {
      if (startupTimer) {
        clearTimeout(startupTimer);
      }
      if (!startupRejected) {
        startupRejected = true;
        startupReject(new Error("rtsp publish receiver exited"));
      }
      if (coreStream) {
        coreStream.request.end();
      }
      connection.state = "closed";
      await this.upsertStream(connection.streamKey, false);
      this.activeSessions.delete(connection.sessionId);
      await this.removeCoreSession(connection);
    });
    await startupPromise;
  }

  async startRecord(connection) {
    if (!connection.sdp || !connection.serverPorts) {
      throw new Error("SETUP required before RECORD");
    }
    await this.ensureReceiverProcess(connection);
    connection.recordStartedAt = new Date().toISOString();
    connection.state = "recording";
    this.activeSessions.set(connection.sessionId, connection);
    await this.upsertStream(connection.streamKey, true);
    await this.syncCoreSession(connection);
  }

  async prepareUdpSockets(connection) {
    if (connection.portHoldRtp && connection.portHoldRtcp) {
      return;
    }
    const pair = await bindUdpSocketPair(this.host);
    connection.portHoldRtp = pair.rtpSocket;
    connection.portHoldRtcp = pair.rtcpSocket;
    connection.serverPorts = {
      rtp: pair.rtpPort,
      rtcp: pair.rtcpPort
    };
  }

  async handleRequest(connection, raw) {
    const { method, uri, headers, body } = this.parseRequest(raw);
    const cseq = headers.cseq || "1";
    const baseHeaders = {
      CSeq: cseq,
      Server: "OTTS-RTSP-PUBLISH/0.1"
    };

    if (method === "OPTIONS") {
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Public: "OPTIONS, ANNOUNCE, SETUP, RECORD, TEARDOWN"
      });
      return;
    }

    if (uri && !connection.streamKey) {
      connection.streamPath = normalizeRtspMount(uri);
      if (connection.streamPath) {
        connection.streamKey = streamKeyFromUri(uri);
      }
    }

    if (!connection.authorized) {
      const authStreamKey = connection.streamKey || streamKeyFromUri(uri);
      if (!isStreamTokenAuthorizedFromUri(uri, "publish", authStreamKey)) {
        connection.state = "unauthorized";
        this.send(connection, "401 Unauthorized", baseHeaders);
        return;
      }
      connection.authorized = true;
    }

    if (method === "ANNOUNCE") {
      connection.sdp = parseSdp(body);
      connection.announcedAt = new Date().toISOString();
      connection.state = "announced";
      await this.syncCoreSession(connection);
      this.send(connection, "200 OK", baseHeaders);
      return;
    }

    if (method === "SETUP") {
      const ports = parseTransportPorts(headers.transport || "");
      if (!ports) {
        this.send(connection, "461 Unsupported Transport", baseHeaders);
        return;
      }
      connection.clientPorts = ports;
      await this.prepareUdpSockets(connection);
      await this.ensureReceiverProcess(connection);
      connection.state = "setup";
      await this.syncCoreSession(connection);
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Session: connection.sessionId,
        Transport:
          `RTP/AVP/UDP;unicast;client_port=${ports.rtp}-${ports.rtcp};server_port=${connection.serverPorts.rtp}-${connection.serverPorts.rtcp}`
      });
      return;
    }

    if (method === "RECORD") {
      await this.startRecord(connection);
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Session: connection.sessionId
      });
      return;
    }

    if (method === "TEARDOWN") {
      this.cleanupConnection(connection);
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Session: connection.sessionId
      });
      return;
    }

    this.send(connection, "405 Method Not Allowed", baseHeaders);
  }

  async cleanupConnection(connection) {
    if (connection.ffmpeg && !connection.ffmpeg.killed) {
      connection.ffmpeg.kill("SIGTERM");
    }
    if (connection.portHoldRtp) {
      try {
        connection.portHoldRtp.close();
      } catch {
        // ignore
      }
    }
    if (connection.portHoldRtcp) {
      try {
        connection.portHoldRtcp.close();
      } catch {
        // ignore
      }
    }
    if (connection.sdpPath) {
      try {
        await fs.unlink(connection.sdpPath);
      } catch {
        // ignore
      }
    }
    if (connection.streamKey) {
      await this.upsertStream(connection.streamKey, false);
    }
    this.activeSessions.delete(connection.sessionId);
    await this.removeCoreSession(connection);
    connection.ffmpeg = null;
    connection.portHoldRtp = null;
    connection.portHoldRtcp = null;
    connection.state = "closed";
  }
}
