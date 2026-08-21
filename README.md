# OTTS (Over-The-Top Stream)

OTTS is an open-source streaming server for Ubuntu. It uses a C++20 media core,
a Node.js control plane and web console, and Python for smoke tests and optional
compatibility tooling.

The current development snapshot supports H.264 and H.265/HEVC in one shared
media model. HEVC has been validated through SRT MPEG-TS, native RTSP, Enhanced
RTMP, HTTP-FLV, HLS fMP4, and native WHIP/WHEP. Native WebRTC uses Opus on the
wire and AAC in the shared core. For reliable browser playback, OTTS can
transcode HEVC to low-latency H.264 with FFmpeg/libx264; direct H.265 WebRTC is
also available for compatible peers and low-delay HEVC sources.

## Current Status

| Protocol | Publish | Play | H.264 | H.265 | Current path |
| --- | --- | --- | --- | --- | --- |
| RTMP | Yes | Yes | Yes | Enhanced RTMP | Native C++ |
| HTTP-FLV | - | Yes | Yes | Enhanced FLV | Native C++ remux |
| HLS | - | Yes | MPEG-TS | fMP4 | Node-managed FFmpeg segmenter |
| WHIP | Yes | - | Yes | Yes when peer offers H265 | Native C++ / libdatachannel |
| WHEP | - | Yes | Yes | Direct H265 for low-delay sources or H.264 fallback | Native C++ / libdatachannel |
| RTSP | Yes | Yes | Yes | RFC 7798 RTP | Native C++ RTP ingest and egress |
| SRT | Yes | Yes | Yes | MPEG-TS stream type `0x24` | Native C++ libsrt + MPEG-TS |

Implemented runtime features include:

- unified stream registry, tracks, packet statistics, GOP cache, and fanout;
- AVC/HEVC parameter-set caching, DTS/PTS handling, GOP startup, and fanout;
- shared Annex-B, length-prefixed, AVCDecoderConfigurationRecord, and hvcC conversion;
- stream health, session, disconnect, and Prometheus-style metrics APIs;
- SRS-compatible WHIP/WHEP HTTP endpoints;
- per-session Opus/AAC audio transcoding between WebRTC and the shared core;
- RTSP H.264/H.265 plus AAC RTP publish and UDP/TCP-interleaved play;
- SRT listener-mode ingest and dynamic multi-session egress routing;
- HTTP-FLV slow-client protection and connection statistics;
- HLS MPEG-TS/fMP4 selection, master playlist, restart, and cleanup management;
- on-demand HEVC-to-H.264 WHEP fallback using libavcodec and libx264;
- FLV/MP4 recording lifecycle;
- token and HMAC-signed stream authorization;
- browser dashboard and WHIP/WHEP test page.

## Architecture

```text
Publish clients
  RTMP / WHIP / RTSP / SRT
              |
              v
      C++ protocol adapters
              |
              v
   StreamRegistry + MediaPacket
     tracks / GOP / fanout / stats
              |
              v
Play clients
  RTMP / HTTP-FLV / WHEP / RTSP / SRT
              |
              +--> Node control plane: dashboard, HLS, recording, callbacks
              +--> Python: smoke tests and optional WebRTC fallback tooling
```

Runtime ownership:

- `cpp/`: C++20 media data plane and HTTP API;
- `node/`: Express control plane, dashboard, HLS, and recording workers;
- `python/`: automated tests and optional compatibility gateway;
- `scripts/`: build-independent operations and regression tests;
- `config/`: runtime environment and JSON configuration examples.

The main C++ executable supervises the Node control plane. With the native
libdatachannel build used by the current stable environment, Python is not in
the live WHIP/WHEP media path.

## Validated Platform

The current server has been built and tested on:

- Ubuntu 24.04.4 LTS x86_64;
- GCC with C++20;
- CMake 3.20 or newer;
- FFmpeg/ffplay;
- libsrt 1.5.3;
- OpenSSL 3.0;
- libnice 0.1.21;
- libsrtp2 2.5;
- libusrsctp;
- Node.js and npm.

Ubuntu 22.04 may also work, but some package names and versions differ.

## Install Build Dependencies

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git \
  ffmpeg curl openssl psmisc iproute2 \
  nodejs npm python3 \
  libssl-dev libsrt-openssl-dev \
  libnice-dev libsrtp2-dev libusrsctp-dev \
  libavcodec-extra libavcodec-dev libavutil-dev libswresample-dev
```

Install the Node dependency:

```bash
cd node
npm ci
cd ..
```

Python packages `aiohttp`, `aiortc`, and `av` are only required by the optional
Python WebRTC gateway and WebRTC smoke tools. On Ubuntu they can be installed
from distribution packages when available, or in a virtual environment.

## Build

### Native RTMP, HTTP-FLV, RTSP, SRT, and HEVC codec model

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

### Native WHIP/WHEP

Build and install libdatachannel under the ignored `third_party/` directory:

```bash
git clone --recursive \
  https://github.com/paullouisageneau/libdatachannel.git \
  third_party/libdatachannel-src

cmake -S third_party/libdatachannel-src \
  -B third_party/libdatachannel-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_NICE=ON \
  -DNO_EXAMPLES=ON \
  -DNO_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/third_party/libdatachannel-install"

cmake --build third_party/libdatachannel-build \
  --target install -j"$(nproc)"
```

Configure OTTS with the native backend:

```bash
cmake -S . -B build \
  -DOTTS_ENABLE_WEBRTC_DATACHANNEL=ON \
  -DOTTS_LIBDATACHANNEL_ROOT="$PWD/third_party/libdatachannel-install"

cmake --build build -j"$(nproc)"
```

The libdatachannel build also enables native WHIP/WHEP, Opus/AAC conversion,
and the HEVC-to-H.264 fallback. The resulting executable is:

```text
build/otts_rtmp
```

## Configure

Create local runtime files from the tracked examples:

```bash
cp config/otts.env.example config/otts.env
cp config/otts.config.json.example config/otts.config.json
```

Edit at least these values for the deployment host:

```bash
OTTS_PUBLIC_HOST=192.168.40.11
OTTS_RTSP_PUBLIC_HOST=192.168.40.11
OTTS_SRT_PUBLIC_HOST=192.168.40.11
```

The local files `config/otts.env` and `config/otts.config.json` are ignored by
Git. Shell environment variables override values loaded from
`config/otts.env`.

Detailed vhost, callback, recording, HLS, and hot-reload behavior is documented
in `docs/config-and-callbacks.md`.

## Start And Stop

Use the operational scripts for normal development:

```bash
bash scripts/restart_otts.sh
bash scripts/status_otts.sh
bash scripts/stop_otts.sh
```

`restart_otts.sh` performs dependency/port checks, stops the previous OTTS
process tree, launches the latest `build/otts_rtmp`, and prints health,
streams, sessions, listeners, and recent logs.

Direct foreground start is useful for debugging:

```bash
./build/otts_rtmp
```

## Default Ports

| Port | Transport | Service |
| --- | --- | --- |
| 1935 | TCP | RTMP |
| 1985 | TCP | WHIP/WHEP compatibility HTTP |
| 3000 | TCP | Node HTTP dashboard |
| 3443 | TCP | Node HTTPS dashboard |
| 8080 | TCP | C++ API and HTTP-FLV |
| 8081 | TCP | Optional Python WebRTC fallback |
| 8554 | TCP + dynamic UDP | RTSP publish |
| 8556 | TCP + dynamic UDP | RTSP play |
| 9000 | UDP | SRT publish |
| 10000 | UDP | SRT play |

Open firewall access for the ports and transports used by your clients. RTSP
over UDP also negotiates dynamic RTP/RTCP UDP ports.

## Protocol Verification

Replace `192.168.40.11` and the media filename as needed.

### Cross-Protocol Fanout

One publisher can feed multiple protocol outputs through the shared C++ stream
registry. Publish one H.264/AAC stream:

```bash
ffmpeg -re -i news_1280x720.mp4 \
  -c copy -f flv \
  rtmp://192.168.40.11:1935/live/nolo001
```

The same `live/nolo001` stream can then be played concurrently through:

```text
RTMP: rtmp://192.168.40.11:1935/live/nolo001
RTSP: rtsp://192.168.40.11:8556/live__nolo001.sdp
WHEP: http://192.168.40.11:1985/rtc/v1/whep/?app=live&stream=nolo001
SRT:  srt://192.168.40.11:10000?mode=caller&transtype=live&streamid=live/nolo001
```

This path performs remuxing and protocol conversion, not video transcoding.

### H.265 / HEVC Cross-Protocol Fanout

Use SRT MPEG-TS for a portable HEVC ingest test. The Haivision-style stream ID
routes this publisher to `live/hevc-demo` without changing server configuration:

```bash
ffmpeg -re -i news_1280x720.mp4 \
  -c:v libx265 -preset ultrafast -pix_fmt yuv420p -g 50 -bf 0 \
  -c:a aac -f mpegts \
  "srt://192.168.40.11:9000?mode=caller&transtype=live&streamid=#!::r=live/hevc-demo,m=publish"
```

The same HEVC/AAC stream can be read concurrently from:

```text
SRT:       srt://192.168.40.11:10000?mode=caller&transtype=live&streamid=#!::r=live/hevc-demo,m=request
RTMP:      rtmp://192.168.40.11:1935/live/hevc-demo
HTTP-FLV:  http://192.168.40.11:8080/live/hevc-demo.flv
RTSP:      rtsp://192.168.40.11:8556/live__hevc-demo.sdp
HLS:       http://192.168.40.11:3000/hls/live/hevc-demo/master.m3u8
WHEP:      http://192.168.40.11:1985/rtc/v1/whep/?app=live&stream=hevc-demo
```

Enhanced RTMP and HTTP-FLV carry an `hvc1` FourCC and hvcC configuration with
length-prefixed coded NAL units. Client support varies; current FFmpeg builds
can be used for protocol verification. HEVC HLS uses `init.mp4` and `.m4s`
segments with `EXT-X-MAP` instead of MPEG-TS segments.

`-bf 0` produces a low-delay HEVC stream suitable for direct browser WHEP.
File-oriented HEVC commonly contains B-frames and remains valid for RTMP, SRT,
RTSP, HTTP-FLV, and HLS, but some browser WebRTC H.265 decoders discard those
B-frames. In that case, use the default H.264 WHEP preference on the test page.

### RTMP

Publish:

```bash
ffmpeg -re -i news_1280x720.mp4 \
  -c copy -f flv \
  rtmp://192.168.40.11:1935/live/nolo001
```

Play:

```bash
ffplay rtmp://192.168.40.11:1935/live/nolo001
```

### HTTP-FLV

```bash
ffplay http://192.168.40.11:8080/live/nolo001.flv
```

Browser playback is provided by the dashboard using flv.js.

### HLS

```text
http://192.168.40.11:3000/hls/live/nolo001/index.m3u8
http://192.168.40.11:3000/hls/live/nolo001/master.m3u8
```

### WHIP / WHEP

WHIP publish URL:

```text
http://192.168.40.11:1985/rtc/v1/whip/?app=live&stream=livestream
```

WHEP play URL:

```text
http://192.168.40.11:1985/rtc/v1/whep/?app=live&stream=livestream
```

The first URL can be used by OBS WHIP output. The browser test page is:

```text
https://192.168.40.11:3443/webrtc.html
```

WebRTC negotiates Opus audio with OBS and browsers. OTTS respects the video
codec order in the peer SDP: H.265 is selected when it is the peer's preferred
offered codec, otherwise H.264 is selected. OTTS stores WHIP audio as AAC in
the shared stream registry, allowing the same stream to be consumed by
AAC-based outputs. WHEP accepts AAC from RTMP, RTSP, SRT, or WHIP sources and
transcodes it to Opus for the WebRTC receiver.

The browser test page defaults to `H.264 (stable)` playback. When the source is
HEVC, OTTS lazily starts an HEVC-to-H.264 transcoder and begins decoding from a
cached parameter-set keyframe. The page also exposes an explicit H.265 direct
option for compatible browsers, GPUs, and low-delay HEVC streams without
B-frames. This choice changes only the WebRTC output and keeps the same WHEP
URL and shared source stream.

The WHEP sender preserves the complete cached GOP and AVC sequence headers,
paces output by media timestamps, and waits for the next keyframe after a slow
client queue overflow. The browser test page reports decoded FPS, receive
bitrate, packet loss, dropped frames, jitter, and jitter-buffer delay. For a
1280x720 30 fps input, H.264-compatible playback should normally report
approximately 30 decoded fps with zero packet loss. Direct H.265 results depend
on browser, GPU, HEVC profile, and whether the source contains B-frames.

When the configured certificate files do not exist, the Node control plane
uses OpenSSL to generate a local self-signed development certificate. The key
is ignored by Git. A browser warning is expected until a trusted certificate is
configured.

### RTSP

The native RTSP path supports H.264 or H.265 video with AAC audio. H.265 uses
RFC 7798 single-NAL, aggregation-packet, and fragmentation-unit RTP handling.
Publish to port 8554 and play from port 8556. A stream key containing `/` is
represented as `__` in the RTSP path.

Publish:

```bash
ffmpeg -re -stream_loop -1 -i 1.mkv \
  -rtsp_transport udp \
  -c:v copy -c:a copy -f rtsp \
  rtsp://192.168.40.11:8554/live__rtsp-compat.sdp
```

Play:

```bash
ffplay rtsp://192.168.40.11:8556/live__rtsp-compat.sdp
```

The RTSP implementation parses H.264/H.265 parameter sets, AAC SDP and RFC 3640
AU headers, publishes both tracks into the shared registry, and preserves DTS,
PTS, and RTP presentation timestamps for B-frame playback.

### SRT

Publish MPEG-TS:

```bash
ffmpeg -re -i news_1280x720.mp4 \
  -c copy -f mpegts \
  "srt://192.168.40.11:9000?mode=caller&transtype=live"
```

Play:

```bash
ffplay "srt://192.168.40.11:10000?mode=caller&transtype=live&streamid=live/nolo001"
```

`streamid` selects any online `app/stream` from the shared C++ stream registry,
so an RTMP publisher at `live/nolo001` can be pulled through SRT without changing
server configuration. The play listener supports concurrent clients and different
stream IDs on the same UDP port. OTTS also accepts Haivision-style stream IDs such
as `#!::r=live/nolo001,m=request`; when `streamid` is omitted, the configured
`OTTS_CPP_SRT_STREAM_KEY` remains the compatibility fallback.
MPEG-TS PMT stream types `0x1b` (AVC) and `0x24` (HEVC) are discovered
dynamically; OTTS does not assume fixed elementary PIDs.

## Web Console And APIs

Dashboard:

```text
http://192.168.40.11:3000/
https://192.168.40.11:3443/
```

Core endpoints:

```bash
curl http://127.0.0.1:8080/api/health
curl http://127.0.0.1:8080/api/streams
curl http://127.0.0.1:8080/api/sessions
curl http://127.0.0.1:8080/metrics
```

Disconnect a stream:

```bash
curl -X POST \
  "http://127.0.0.1:8080/api/streams/disconnect?stream_key=live/demo"
```

The stream API reports publisher state, codecs, packet/byte rates, viewers,
sequence headers, keyframes, GOP cache, and HTTP-FLV counters.

## Recording

```bash
curl -X POST \
  "http://127.0.0.1:3000/api/recordings/start?stream_key=live/demo&format=mp4"

curl -X POST \
  "http://127.0.0.1:3000/api/recordings/stop?stream_key=live/demo"

curl http://127.0.0.1:3000/api/recordings
```

The default recording directory is `/tmp/otts_recordings`.

## Authentication

Open development mode is the default. To require a shared token, set
`OTTS_STREAM_TOKEN` in `config/otts.env` and restart OTTS.

For per-action signed URLs, set `OTTS_AUTH_SECRET` and generate a URL with:

```bash
python3 scripts/sign_stream_url.py \
  live/nolo001 --ttl 3600 --host 192.168.40.11
```

Publish and play signatures are different and cannot be reused. RTSP accepts
the token/signature in the URL query. SRT uses its passphrase and `pbkeylen`
parameters when edge encryption is enabled.

## Smoke And Regression Tests

Preflight:

```bash
bash scripts/preflight_otts.sh
bash scripts/preflight_otts.sh --strict
```

Individual protocol tests:

```bash
bash scripts/smoke_rtmp.sh live/rtmp-smoke
bash scripts/smoke_rtsp.sh /path/to/input.mp4 live/rtsp-smoke
bash scripts/smoke_srt.sh /path/to/input.mp4 live/srt-smoke
bash scripts/smoke_hls.sh live/hls-smoke
python3 python/smoke_webrtc_native.py \
  --base-url http://127.0.0.1:1985 \
  --stream-key live/webrtc-smoke --duration 8
```

Run the complete H.265 cross-protocol regression using a generated test source:

```bash
bash scripts/smoke_hevc.sh
```

This verifies HEVC+AAC over SRT, Enhanced RTMP, HTTP-FLV, RTSP, and HLS fMP4,
checks H.265 WHIP/WHEP SDP negotiation, and exercises WHEP HEVC-to-H.264
fallback with decoded video and audio frames.

Test WHEP playback against an already-published stream:

```bash
python3 python/smoke_webrtc_native.py \
  --base-url http://127.0.0.1:1985 \
  --stream-key live/nolo001 --duration 10 --play-only
```

The WebRTC smoke test verifies both video and audio and reports video/audio
frame counts plus P50, P95, and maximum receive intervals.

Full suite:

```bash
bash scripts/smoke_all.sh /path/to/input.mp4
```

Smoke scripts use isolated test stream keys and do not intentionally stop
unrelated user streams.

## Runtime Files

Common diagnostics:

```text
/tmp/otts_core.pid
/tmp/otts_clean2.out
/tmp/otts_clean2.err
/tmp/otts_node.out
/tmp/otts_node.err
/tmp/otts_state.json
```

Build directories, local configuration, test media, archives, logs,
`node_modules`, and `third_party/` are ignored by Git.

## Current Limits

- Video processing is remux/passthrough except for the on-demand WHEP
  HEVC-to-H.264 fallback. Audio is converted between WebRTC Opus and core AAC.
- The tested HEVC profile is 8-bit 4:2:0. Client support for Enhanced RTMP,
  HTTP-FLV HEVC, and WebRTC HEVC varies by player, browser, OS, and GPU.
- Native HEVC-to-H.264 fallback is pooled by stream and transcode settings.
  One HEVC decoder and one H.264 encoder feed a shared encoded GOP cache; each
  WHEP session keeps independent RTP/RTCP, SSRC, congestion control, and send
  queue state. A different bitrate, frame rate, GOP, or encoder profile creates
  a separate shared pipeline.
- Direct H.265 WHEP is intended for low-delay HEVC without B-frames. Use the
  default H.264 WHEP preference for file-oriented HEVC containing B-frames.
- Cluster forwarding, replay, and production certificate automation remain
  future work.
- HLS and recording still use managed FFmpeg workers.
- SRT callers that omit `streamid` use the configured compatibility fallback
  stream.

## License And Contributions

Before publishing a public release, add the intended open-source `LICENSE`
file and verify third-party license obligations for bundled binaries and
release archives. Issues and pull requests should include reproduction
commands, OTTS logs, client versions, and the relevant stream API snapshot.
