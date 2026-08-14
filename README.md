# OTTS

OTTS, short for `Over-The-Top Stream`, is an open-source streaming server project for Ubuntu deployments.

Current phase focus:

- `C++` media core
- `Node.js` control plane and web console
- `Python` WebRTC gateway and test tooling

The current stable phase already covers:

- RTMP publish/play
- HTTP-FLV playback
- HLS output
- WHIP/WHEP compatibility access
- RTSP publish/play compatibility
- SRT publish/play compatibility
- live stream status APIs
- browser management console

## Architecture

OTTS is split into three layers:

- `cpp/`: media core, RTMP server, HTTP API, stream registry
- `node/`: control plane, dashboard, HLS/RTSP/SRT worker orchestration
- `python/`: WebRTC gateway, smoke tools, helper utilities

Current design principle:

- C++ handles the media data plane
- Node.js handles control plane and web UI
- Python handles WebRTC gateway and automation helpers

## Repository Layout

- `cpp/`: C++ core implementation
- `node/`: Express-based control plane and web UI
- `python/`: WebRTC gateway and helper scripts
- `scripts/`: operational scripts for restart, status, stop, smoke tests
- `docs/`: notes and staged design documents

## Stable Features In This Version

- RTMP ingest from OBS / FFmpeg
- RTMP playback from ffplay and compatible clients
- H.264 + AAC passthrough
- GOP cache and metadata / sequence-header caching
- HTTP-FLV playback for browser and player testing
- HLS playlist generation
- SRS-style WHIP/WHEP compatibility URLs
- RTSP publish/play compatibility endpoints
- SRT publish/play compatibility endpoints
- C++ health, stream list, disconnect, and session APIs
- Node dashboard for stream visibility and protocol debugging

## Not Yet Native In C++ Core

These protocol paths are currently stable, but still rely on bridge workers outside the pure C++ media path:

- WHIP/WHEP media bridging
- RTSP compatibility media bridging
- SRT compatibility media bridging
- HLS segment generation

That is acceptable for the current phase-1/phase-2 stable milestone.

## Environment

Recommended OS:

- Ubuntu 22.04 or compatible Ubuntu environment

Recommended runtime dependencies:

- `build-essential`
- `cmake`
- `ffmpeg`
- `python3`
- `python3-aiohttp`
- `python3-aiortc`
- `python3-av`
- `nodejs`
- `npm`

## Build Dependencies

Install once on Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ffmpeg \
  python3 \
  python3-aiohttp \
  python3-aiortc \
  python3-av \
  nodejs \
  npm
```

Install Node dependencies once:

```bash
cd /home/nolovr/Downloads/otts/node
npm install
```

## Build

```bash
cd /home/nolovr/Downloads/otts
cmake -S . -B build
cmake --build build -j
```

Main executable:

```bash
/home/nolovr/Downloads/otts/build/otts_rtmp
```

## Run

The OTTS executable is the single launcher entry for the current version. It starts:

- C++ RTMP core
- Node.js control plane
- Python WebRTC gateway

Recommended start command:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/restart_otts.sh
```

Direct start is also possible:

```bash
cd /home/nolovr/Downloads/otts
./build/otts_rtmp
```

But for day-to-day use, `restart_otts.sh` is preferred because it first clears old processes and occupied ports.

## Operations

Restart OTTS cleanly:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/restart_otts.sh
```

Check current status:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/status_otts.sh
```

Stop all OTTS-related processes:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/stop_otts.sh
```

## Default Ports

- RTMP: `1935`
- C++ HTTP API / HTTP-FLV: `8080`
- WHIP/WHEP compatibility HTTP: `1985`
- Python WebRTC gateway: `8081`
- Node HTTP console: `3000`
- Node HTTPS console: `3443`
- RTSP publish: `8554`
- RTSP play: `8556`
- SRT publish: `9000`
- SRT play: `10000`

## Web Console

Open:

- `http://127.0.0.1:3000`
- `https://127.0.0.1:3443`

The dashboard includes:

- live stream list
- protocol session views
- HLS status
- RTSP compatibility status
- SRT status
- WHIP/WHEP test page
- playback URL references

## Core HTTP APIs

Health:

```bash
curl http://127.0.0.1:8080/api/health
```

Current streams:

```bash
curl http://127.0.0.1:8080/api/streams
```

Current protocol sessions:

```bash
curl http://127.0.0.1:8080/api/sessions
```

Disconnect a stream:

```bash
curl -X POST "http://127.0.0.1:8080/api/streams/disconnect?stream_key=live/demo"
```

SRT session status:

```bash
curl http://127.0.0.1:3000/api/srt/sessions
```

## Protocol Test Matrix

The following commands are the validated compatibility tests for the current stable version.

### RTMP

Push:

```bash
ffmpeg -re -i news_1280x720.mp4 -c copy -f flv rtmp://192.168.40.11:1935/live/nolo001
```

Play:

```bash
ffplay rtmp://192.168.40.11:1935/live/nolo001
```

Local smoke test:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/smoke_rtmp.sh
```

### HTTP-FLV

Play URL example:

```text
http://192.168.40.11:8080/live/nolo001.flv
```

### HLS

Playlist URL examples:

```text
http://192.168.40.11:3000/hls/live__nolo001/index.m3u8
http://192.168.40.11:3000/hls/live__nolo001/master.m3u8
```

### WHIP / WHEP

Validated compatibility URLs:

- WHIP publish:
  `http://192.168.40.11:1985/rtc/v1/whip/?app=live&stream=livestream`
- WHEP play:
  `http://192.168.40.11:1985/rtc/v1/whep/?app=live&stream=livestream`

Web test page:

```text
https://192.168.40.11:3443/webrtc.html
```

### RTSP

Push:

```bash
ffmpeg -re -i news_1280x720.mp4 -rtsp_transport udp -vcodec h264 -f rtsp rtsp://192.168.40.11:8554/live__rtsp-compat.sdp
```

Play:

```bash
ffplay rtsp://192.168.40.11:8556/live__rtsp-compat.sdp
```

Local smoke test:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/smoke_rtsp.sh news_1280x720.mp4
```

### SRT

Push:

```bash
ffmpeg -re -i news_1280x720.mp4 -c copy -f mpegts "srt://192.168.40.11:9000?mode=caller&transtype=live"
```

Play:

```bash
ffplay "srt://192.168.40.11:10000?mode=caller&transtype=live"
```

Local smoke test:

```bash
cd /home/nolovr/Downloads/otts
bash scripts/smoke_srt.sh news_1280x720.mp4
```

OTTS bootstraps the default `live/srt-demo` SRT listener pair on startup, so the default `9000/10000` endpoints are available immediately after service start.

## Testing Recommendation

Recommended validation order:

1. Run `bash scripts/restart_otts.sh`
2. Check `bash scripts/status_otts.sh`
3. Verify RTMP
4. Verify HTTP-FLV
5. Verify HLS
6. Verify WHIP/WHEP
7. Verify RTSP
8. Verify SRT

If you need a quick sanity check before external testing:

- `bash scripts/smoke_rtmp.sh`
- `bash scripts/smoke_rtsp.sh news_1280x720.mp4`
- `bash scripts/smoke_srt.sh news_1280x720.mp4`

## Runtime Files

Runtime state file:

```text
/tmp/otts_state.json
```

Managed child process logs:

```text
/tmp/otts_clean2.out
/tmp/otts_clean2.err
/tmp/otts_node.out
/tmp/otts_node.err
/tmp/otts_webrtc.out
/tmp/otts_webrtc.err
```

## Current Milestone Summary

This version is suitable as a stable pre-commit milestone for:

- RTMP
- HTTP-FLV
- HLS
- WHIP/WHEP compatibility access
- RTSP compatibility access
- SRT compatibility access

## Next Planned Work

1. Continue moving SRT media path into the native C++ core.
2. Continue moving RTSP media path into the native C++ core.
3. Bridge WHIP/WHEP media path into the unified C++ stream model.
4. Add authentication, recording, and stronger packaging for open-source release.
