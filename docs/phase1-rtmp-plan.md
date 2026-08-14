# Phase 1 RTMP Plan

## Goal

Build the first usable server slice around a shared media core:

1. RTMP publish
2. RTMP play
3. stream registry
4. packet fanout
5. basic control plane visibility

## Architecture

### Data Plane

- `RtmpServer`: accepts TCP connections
- `RtmpSession`: owns handshake, chunk parsing, command handling, and media ingest/egress
- `StreamRegistry`: maps stream keys to one publisher and many subscribers
- `MediaMessage`: the normalized unit passed across the RTMP layer

### Control Plane

- `Node.js` service exposes HTTP endpoints and a basic dashboard
- initial version uses mock in-memory data
- next version should proxy to the C++ core over HTTP or gRPC

## Implementation Order

1. TCP listener and per-session lifecycle
2. RTMP handshake
3. chunk parser and chunk writer
4. AMF0 command decode/encode
5. `connect/createStream/publish/play`
6. stream registry and media fanout
7. smoke tests with FFmpeg/ffplay/OBS

## Follow-up Features

- authentication
- HLS/HTTP-FLV
- RTSP ingest/egress
- SRT ingest/egress
- WHIP/WHEP
- metrics and tracing
