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

## Stream Token Authentication

By default, OTTS keeps the local development behavior open and does not require a stream token.
Set `OTTS_STREAM_TOKEN` before starting the core to require the same `token` query parameter on RTMP publish/play, HTTP-FLV playback, and WHIP/WHEP publish/play requests.

Example:

```bash
export OTTS_STREAM_TOKEN=change-me
bash scripts/restart_otts.sh
```

Authenticated URLs:

```bash
rtmp://192.168.40.11:1935/live/nolo001?token=change-me
http://192.168.40.11:8080/live/nolo001.flv?token=change-me
http://192.168.40.11:1985/rtc/v1/whip/?app=live&stream=livestream&token=change-me
http://192.168.40.11:1985/rtc/v1/whep/?app=live&stream=livestream&token=change-me
```

HLS compatibility workers enter the core through managed RTMP loopback paths and automatically carry the token internally.
RTSP edge listeners require `token` in the RTSP URL when `OTTS_STREAM_TOKEN` is set.
SRT edge listeners use libsrt connection encryption as edge auth: when `OTTS_STREAM_TOKEN` is 10-79 characters, clients must add `passphrase=<token>&pbkeylen=16`.

RTSP/SRT examples with authentication enabled:

```bash
ffmpeg -re -i news_1280x720.mp4 -rtsp_transport udp -vcodec h264 -f rtsp "rtsp://192.168.40.11:8554/live__rtsp-compat.sdp?token=change-me"
ffplay "rtsp://192.168.40.11:8556/live__rtsp-compat.sdp?token=change-me"

ffmpeg -re -i news_1280x720.mp4 -c copy -f mpegts "srt://192.168.40.11:9000?mode=caller&transtype=live&passphrase=change-me-10&pbkeylen=16"
ffplay "srt://192.168.40.11:10000?mode=caller&transtype=live&passphrase=change-me-10&pbkeylen=16"
```

## Stream Key Signed URLs

`OTTS_STREAM_TOKEN` remains supported as a simple global token. For stream-key level permission, set `OTTS_AUTH_SECRET`; clients may then use signed URLs with `expires` and `sign`. The signature is HMAC-SHA256 over `action + "\n" + stream_key + "\n" + expires`, where action is `publish` or `play`.

Generate signed test URLs on the server:

```bash
export OTTS_AUTH_SECRET=change-this-secret
python3 scripts/sign_stream_url.py live/nolo001 --ttl 3600 --host 192.168.40.11
```

A publish signature cannot be reused for play, and an expired signature is rejected by RTMP, HTTP-FLV, WHIP/WHEP, and RTSP edge entry points.

## Configuration File

Runtime settings can be centralized in `config/otts.env`. Start from the example file:

```bash
cp config/otts.env.example config/otts.env
vim config/otts.env
bash scripts/restart_otts.sh
```

Shell environment variables still have priority over values in `config/otts.env`, so temporary overrides work as expected:

```bash
OTTS_AUTH_SECRET=test-secret bash scripts/restart_otts.sh
```

The shared loader is `scripts/otts_env.sh`; `restart_otts.sh`, `stop_otts.sh`, `status_otts.sh`, and `scripts/sign_stream_url.py` use the same configuration defaults.

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

## Recording

Recordings are managed by the Node control plane and saved under `/tmp/otts_recordings` by default.
The first implementation records from the RTMP core and supports `flv` and `mp4` outputs.

Start recording:

```bash
curl -X POST "http://127.0.0.1:3000/api/recordings/start?stream_key=live/demo&format=flv"
curl -X POST "http://127.0.0.1:3000/api/recordings/start?stream_key=live/demo&format=mp4"
```

Stop recording:

```bash
curl -X POST "http://127.0.0.1:3000/api/recordings/stop?stream_key=live/demo"
```

List recording files:

```bash
curl http://127.0.0.1:3000/api/recordings
```

## Metrics And Log Level

The C++ core exposes Prometheus-style metrics on the HTTP API port. Use this for dashboards, alerts, or quick runtime checks:

```bash
curl http://127.0.0.1:8080/metrics
curl http://127.0.0.1:8080/api/metrics
```

Useful metrics include current stream count, publisher state, per-stream packets/bytes/viewers, protocol sessions, and HTTP-FLV connection counters.

Set the core log threshold with `OTTS_LOG_LEVEL` in `config/otts.env` or the shell environment:

```bash
OTTS_LOG_LEVEL=debug|info|warn|error
```

## Preflight And Smoke Tests

Run dependency and port checks before troubleshooting a failed start:

```bash
scripts/preflight_otts.sh
scripts/preflight_otts.sh --strict
```

Run smoke tests with the current `config/otts.env` settings. The scripts automatically use configured ports and add token/signature auth when `OTTS_STREAM_TOKEN` or `OTTS_AUTH_SECRET` is enabled:

```bash
scripts/smoke_rtmp.sh live/rtmp-smoke
scripts/smoke_rtsp.sh /path/to/news_1280x720.mp4 live/rtsp-smoke
scripts/smoke_srt.sh /path/to/news_1280x720.mp4 live/srt-smoke
scripts/smoke_all.sh /path/to/news_1280x720.mp4
scripts/smoke_cpp_rtsp_play.sh /path/to/news_1280x720.mp4 live/cpp-rtsp-smoke
```

Smoke tests create their own test streams but do not stop unrelated user streams that are already running.

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
RTSP push defaults to `core-direct-flv`: Node keeps the RTSP control/RTP listener and ffmpeg RTP/FLV demux helper, then streams FLV bytes over one long-lived C++ `/api/internal/media/publish/flv-stream` connection. The C++ core parses FLV tags and injects packets into the unified `StreamRegistry`. Set `OTTS_RTSP_PUBLISH_MODE=legacy-rtmp-loopback` to force the older RTMP bridge path.
C++ native RTSP play owns `OTTS_RTSP_PLAY_PORT=8556` by default and can be tested with `rtsp://192.168.40.11:8556/live__stream.sdp`. It supports H.264 video and AAC audio over RTP/UDP from the C++ `StreamRegistry`.

RTSP play is C++ native on port `8556`: the C++ core handles RTSP control, RTP/UDP, RTP/TCP interleaved, H.264 video, and AAC audio directly from the shared `StreamRegistry`. The older Node/ffmpeg RTSP playback compatibility listener is disabled by `OTTS_RTSP_PLAY_COMPAT_ENABLED=false`.


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
SRT ingest defaults to `core-direct-flv`: Node keeps the SRT listener and ffmpeg MPEG-TS/FLV demux helper, then streams FLV bytes over one long-lived C++ `/api/internal/media/publish/flv-stream` connection. The C++ core parses FLV tags and injects packets into the unified `StreamRegistry` without RTMP loopback. Set `OTTS_SRT_PUBLISH_MODE=legacy-rtmp-loopback` to force the older RTMP bridge path.


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

1. Promote the C++ RTSP play path toward the default `8556` endpoint after longer soak tests.
2. Add RTSP interleaved TCP/RTCP handling, then move RTSP publish control/RTP handling from Node into C++.
3. Move SRT socket handling from Node/ffmpeg into the C++ core when a libsrt integration is added.
4. Keep reducing ffmpeg helper usage toward protocol-native packetization.
5. Bridge WHIP/WHEP media path into the unified C++ stream model.
6. Add management authentication, H.265, snapshots, replay, and multi-node forwarding.

SRT play defaults to `core-egress-flv`: ffmpeg reads the C++ HTTP-FLV output and remuxes it to the SRT MPEG-TS listener. Set `OTTS_SRT_PLAY_MODE=legacy-rtmp-loopback` to force the older RTMP pull path.
