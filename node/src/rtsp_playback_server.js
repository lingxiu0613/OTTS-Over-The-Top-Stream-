import crypto from "crypto";
import dgram from "dgram";
import net from "net";
import { spawn } from "child_process";
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
  if (mount.endsWith("/trackID=0")) {
    mount = mount.slice(0, -"/trackID=0".length);
  }
  if (mount.endsWith(".sdp")) {
    mount = mount.slice(0, -4);
  }
  return mount.replace(/^\/+/, "");
}

function candidateStreamKeys(pathname) {
  const mount = normalizeRtspMount(pathname);
  if (!mount) {
    return [];
  }
  const decoded = (() => {
    try {
      return decodeURIComponent(mount);
    } catch {
      return mount;
    }
  })();
  const candidates = [
    decoded,
    decoded.replaceAll("__", "/"),
    decoded.replaceAll("_", "/"),
    decoded.replaceAll("%2F", "/"),
    decoded.replaceAll("%2f", "/")
  ];
  return [...new Set(candidates.map((item) => item.replace(/^\/+/, "")).filter(Boolean))];
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

export class RtspPlaybackServer {
  constructor(options = {}) {
    this.port = options.port || 8556;
    this.host = options.host || "0.0.0.0";
    this.publicHost = options.publicHost || "127.0.0.1";
    this.apiBase = options.apiBase || "http://127.0.0.1:8080";
    this.rtmpBase = options.rtmpBase || "rtmp://127.0.0.1:1935";
    this.ffmpegBin = options.ffmpegBin || "ffmpeg";
    this.server = null;
    this.connections = new Set();
    this.activeSessions = new Map();
  }

  sessionSummaries() {
    return [...this.activeSessions.values()].map((session) => ({
      stream_key: session.streamKey,
      path: session.streamPath,
      state: session.state,
      transport_mode: session.transportMode,
      client_address: (session.socket?.remoteAddress || "").replace("::ffff:", ""),
      client_rtp_port: session.clientRtpPort || 0,
      interleaved_rtp_channel: session.interleavedRtpChannel,
      ffmpeg_pid: session.ffmpeg?.pid || null,
      play_started_at_epoch_ms: session.playStartedEpochMs || 0
    }));
  }

  sessionKey(connection) {
    return `rtsp-native-play:${connection.sessionId}`;
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
    const params = new URLSearchParams({
      session_key: this.sessionKey(connection),
      stream_key: connection.streamKey,
      source_protocol: "rtsp",
      direction: "play",
      managed_by: "node-rtsp-native-play",
      state: stateOverride || connection.state || "connected",
      public_url: this.publicUrl(connection),
      bind_url: this.bindUrl(connection),
      target_url: `${this.rtmpBase}/${connection.streamKey}`,
      pid: String(connection.ffmpeg?.pid || 0),
      started_at_epoch_ms: String(connection.playStartedEpochMs || 0),
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

  handleConnection(socket) {
    const connection = {
      socket,
      buffer: "",
      sessionId: crypto.randomBytes(8).toString("hex"),
      streamKey: "",
      streamPath: "",
      clientRtpPort: 0,
      clientRtcpPort: 0,
      transportMode: "udp",
      interleavedRtpChannel: 0,
      interleavedRtcpChannel: 1,
      relayRtpSocket: null,
      relayRtcpSocket: null,
      ffmpeg: null,
      state: "connected",
      lastError: "",
      playStartedEpochMs: 0
    };
    this.connections.add(connection);

    socket.on("data", (chunk) => {
      connection.buffer += chunk.toString("utf8");
      this.processBufferedRequests(connection).catch(() => {
        connection.lastError = "request-processing-failed";
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

  cleanupConnection(connection) {
    if (connection.ffmpeg && !connection.ffmpeg.killed) {
      connection.ffmpeg.kill("SIGTERM");
    }
    if (connection.relayRtpSocket) {
      try {
        connection.relayRtpSocket.close();
      } catch {
        // ignore
      }
    }
    if (connection.relayRtcpSocket) {
      try {
        connection.relayRtcpSocket.close();
      } catch {
        // ignore
      }
    }
    this.removeCoreSession(connection).catch(() => {
      // best effort
    });
    this.activeSessions.delete(connection.sessionId);
    connection.ffmpeg = null;
    connection.relayRtpSocket = null;
    connection.relayRtcpSocket = null;
    connection.state = "closed";
  }

  async processBufferedRequests(connection) {
    while (true) {
      const headerEnd = connection.buffer.indexOf("\r\n\r\n");
      if (headerEnd < 0) {
        return;
      }
      const raw = connection.buffer.slice(0, headerEnd + 4);
      connection.buffer = connection.buffer.slice(headerEnd + 4);
      await this.handleRequest(connection, raw);
    }
  }

  parseRequest(raw) {
    const lines = raw.split("\r\n").filter(Boolean);
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
    return { method, uri, headers };
  }

  send(connection, status, headers = {}, body = "") {
    connection.socket.write(buildRtspResponse(status, headers, body));
  }

  async fetchDescribeInfo(streamKeyCandidates) {
    for (const streamKey of streamKeyCandidates) {
      const response = await fetch(`${this.apiBase}/api/rtsp/describe?stream_key=${encodeURIComponent(streamKey)}`);
      if (!response.ok) {
        continue;
      }
      const payload = await response.json();
      return { streamKey, payload };
    }
    return null;
  }

  buildSdp(describeInfo) {
    return [
      "v=0",
      `o=- 0 0 IN IP4 ${this.publicHost}`,
      "s=OTTS RTSP",
      "t=0 0",
      "a=control:*",
      "m=video 0 RTP/AVP 96",
      "c=IN IP4 0.0.0.0",
      "a=rtpmap:96 H264/90000",
      `a=fmtp:96 packetization-mode=1;profile-level-id=${describeInfo.profile_level_id};sprop-parameter-sets=${describeInfo.sprop_parameter_sets}`,
      "a=control:trackID=0",
      ""
    ].join("\r\n");
  }

  async ensureInterleavedRelay(connection) {
    if (connection.relayRtpSocket) {
      return;
    }
    const relayRtpSocket = await bindUdpSocket("127.0.0.1");
    relayRtpSocket.on("message", (packet) => {
      const header = Buffer.alloc(4);
      header[0] = 0x24;
      header[1] = connection.interleavedRtpChannel & 0xff;
      header.writeUInt16BE(packet.length, 2);
      connection.socket.write(Buffer.concat([header, packet]));
    });
    connection.relayRtpSocket = relayRtpSocket;
  }

  async handleRequest(connection, raw) {
    const { method, uri, headers } = this.parseRequest(raw);
    const cseq = headers.cseq || "1";
    const baseHeaders = {
      CSeq: cseq,
      Server: "OTTS-RTSP/0.1"
    };

    if (method === "OPTIONS") {
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Public: "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN"
      });
      return;
    }

    const streamKeyCandidates = candidateStreamKeys(uri);
    if (streamKeyCandidates.length > 0) {
      connection.streamPath = normalizeRtspMount(uri);
      if (!connection.streamKey) {
        connection.streamKey = streamKeyCandidates[0];
      }
    }

    if (method === "DESCRIBE") {
      const describeResult = await this.fetchDescribeInfo(streamKeyCandidates);
      if (!describeResult) {
        console.warn(`[rtsp_playback] describe miss uri=${uri} candidates=${streamKeyCandidates.join(",")}`);
        this.send(connection, "404 Not Found", baseHeaders);
        return;
      }
      connection.streamKey = describeResult.streamKey;
      connection.state = "described";
      await this.syncCoreSession(connection);
      const sdp = this.buildSdp(describeResult.payload);
      this.send(connection, "200 OK", {
        ...baseHeaders,
        "Content-Base": `rtsp://${this.publicHost}:${this.port}/${connection.streamPath}/`,
        "Content-Type": "application/sdp",
        "Content-Length": Buffer.byteLength(sdp)
      }, sdp);
      return;
    }

    if (method === "SETUP") {
      const transport = headers.transport || "";
      const interleavedMatch = transport.match(/interleaved=(\d+)-(\d+)/i);
      if (interleavedMatch) {
        connection.transportMode = "tcp";
        connection.interleavedRtpChannel = Number(interleavedMatch[1]);
        connection.interleavedRtcpChannel = Number(interleavedMatch[2]);
        await this.ensureInterleavedRelay(connection);
        connection.state = "setup";
        await this.syncCoreSession(connection);
        this.send(connection, "200 OK", {
          ...baseHeaders,
          Session: connection.sessionId,
          Transport: `RTP/AVP/TCP;unicast;interleaved=${connection.interleavedRtpChannel}-${connection.interleavedRtcpChannel}`
        });
        return;
      }
      const match = transport.match(/client_port=(\d+)-(\d+)/i);
      if (!match) {
        this.send(connection, "461 Unsupported Transport", baseHeaders);
        return;
      }
      connection.clientRtpPort = Number(match[1]);
      connection.clientRtcpPort = Number(match[2]);
      connection.state = "setup";
      await this.syncCoreSession(connection);
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Session: connection.sessionId,
        Transport: `RTP/AVP/UDP;unicast;client_port=${connection.clientRtpPort}-${connection.clientRtcpPort};server_port=5004-5005`
      });
      return;
    }

    if (method === "PLAY") {
      if (connection.ffmpeg && !connection.ffmpeg.killed) {
        connection.ffmpeg.kill("SIGTERM");
      }
      const remoteIp = (connection.socket.remoteAddress || "").replace("::ffff:", "");
      connection.state = "play-starting";
      connection.ffmpeg = spawn(
        this.ffmpegBin,
        [
          "-hide_banner",
          "-loglevel",
          "info",
          "-fflags",
          "nobuffer",
          "-i",
          `${this.rtmpBase}/${connection.streamKey}`,
          "-map",
          "0:v:0",
          "-an",
          "-c:v",
          "copy",
          "-f",
          "rtp",
          "-payload_type",
          "96",
          connection.transportMode === "tcp"
            ? `rtp://127.0.0.1:${connection.relayRtpSocket.address().port}?pkt_size=1200`
            : `rtp://${remoteIp}:${connection.clientRtpPort}?pkt_size=1200`
        ],
        { stdio: ["ignore", "ignore", "pipe"] }
      );
      connection.ffmpeg.once("spawn", () => {
        connection.playStartedEpochMs = Date.now();
        connection.state = "playing";
        this.activeSessions.set(connection.sessionId, connection);
        this.syncCoreSession(connection).catch(() => {
          // best effort
        });
      });
      connection.ffmpeg.stderr.on("data", (chunk) => {
        const text = chunk.toString("utf8").trim();
        if (text) {
          console.error(`[rtsp_playback] ${connection.streamKey} ${text}`);
        }
      });
      connection.ffmpeg.on("error", (error) => {
        connection.lastError = error.message;
      });
      connection.ffmpeg.on("exit", () => {
        connection.state = "closed";
        this.activeSessions.delete(connection.sessionId);
        this.removeCoreSession(connection).catch(() => {
          // best effort
        });
      });
      this.send(connection, "200 OK", {
        ...baseHeaders,
        Session: connection.sessionId,
        "RTP-Info": "url=trackID=0;seq=0;rtptime=0"
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
}
