#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${OTTS_SMOKE_HOST:-127.0.0.1}"
API_PORT="${OTTS_HTTP_PORT:-8080}"
WEB_PORT="${OTTS_WEB_PORT:-3000}"
COMPAT_PORT="${OTTS_COMPAT_PORT:-1985}"
STREAM_NAME="hevc-smoke-$$"
STREAM_KEY="live/${STREAM_NAME}"
RTSP_KEY="live__${STREAM_NAME}.sdp"
PUBLISH_LOG="/tmp/otts_${STREAM_NAME}_publish.log"

for command in ffmpeg ffprobe curl timeout python3; do
  command -v "${command}" >/dev/null || { echo "[FAIL] missing command: ${command}"; exit 1; }
done

ffmpeg -hide_banner -loglevel warning -re \
  -f lavfi -i testsrc2=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=700:sample_rate=48000 \
  -t 90 -c:v libx265 -preset ultrafast -g 50 -bf 0 -pix_fmt yuv420p \
  -c:a aac -b:a 96k -f mpegts \
  "srt://${HOST}:9000?mode=caller&transtype=live&streamid=#!::r=${STREAM_KEY},m=publish" \
  >"${PUBLISH_LOG}" 2>&1 &
PUBLISH_PID=$!
RTMP_REPUBLISH_PID=""
RTSP_PUBLISH_PID=""
trap 'kill "${PUBLISH_PID}" ${RTMP_REPUBLISH_PID:-} ${RTSP_PUBLISH_PID:-} 2>/dev/null || true' EXIT

echo "[OTTS] waiting for ${STREAM_KEY}"
for _ in $(seq 1 30); do
  if curl -fsS "http://${HOST}:${API_PORT}/api/streams" | grep -q "\"stream_key\":\"${STREAM_KEY}\".*\"video_codec\":\"h265\""; then
    break
  fi
  sleep 1
done
curl -fsS "http://${HOST}:${API_PORT}/api/streams" | grep -q "\"stream_key\":\"${STREAM_KEY}\".*\"ready_for_play\":true"

probe_av() {
  local label="$1"
  shift
  local output
  output="$(timeout 15 ffprobe -v error -show_entries stream=codec_name,codec_type,width,height,r_frame_rate -of compact=p=0:nk=0 "$@")"
  grep -q "codec_name=hevc" <<<"${output}"
  grep -q "codec_name=aac" <<<"${output}"
  echo "[PASS] ${label}: HEVC + AAC"
}

probe_av "SRT MPEG-TS" "srt://${HOST}:10000?mode=caller&transtype=live&streamid=#!::r=${STREAM_KEY},m=request"
probe_av "Enhanced RTMP" "rtmp://${HOST}:1935/${STREAM_KEY}"
probe_av "HTTP-FLV" "http://${HOST}:${API_PORT}/${STREAM_KEY}.flv"
probe_av "RTSP" -rtsp_transport tcp "rtsp://${HOST}:8556/${RTSP_KEY}"

RTMP_INGEST_KEY="live/${STREAM_NAME}-rtmp-ingest"
ffmpeg -hide_banner -loglevel warning -i "rtmp://${HOST}:1935/${STREAM_KEY}" \
  -t 25 -c copy -f flv "rtmp://${HOST}:1935/${RTMP_INGEST_KEY}" \
  >"/tmp/otts_${STREAM_NAME}_rtmp_ingest.log" 2>&1 &
RTMP_REPUBLISH_PID=$!
sleep 3
probe_av "Enhanced RTMP ingest" "rtmp://${HOST}:1935/${RTMP_INGEST_KEY}"

RTSP_INGEST_NAME="${STREAM_NAME}-rtsp-ingest"
ffmpeg -hide_banner -loglevel warning -re \
  -f lavfi -i testsrc2=size=480x270:rate=20 \
  -f lavfi -i sine=frequency=500:sample_rate=48000 \
  -t 30 -c:v libx265 -preset ultrafast -g 40 -bf 0 -pix_fmt yuv420p \
  -c:a aac -rtsp_transport udp -f rtsp \
  "rtsp://${HOST}:8554/live__${RTSP_INGEST_NAME}.sdp" \
  >"/tmp/otts_${STREAM_NAME}_rtsp_ingest.log" 2>&1 &
RTSP_PUBLISH_PID=$!
sleep 4
probe_av "RTSP HEVC ingest" -rtsp_transport tcp \
  "rtsp://${HOST}:8556/live__${RTSP_INGEST_NAME}.sdp"

for _ in $(seq 1 30); do
  if curl -fsS "http://${HOST}:${WEB_PORT}/hls/${STREAM_KEY}/master.m3u8" | grep -q 'CODECS="hvc1'; then
    break
  fi
  sleep 1
done
probe_av "HLS fMP4" "http://${HOST}:${WEB_PORT}/hls/${STREAM_KEY}/master.m3u8"
curl -fsS "http://${HOST}:${WEB_PORT}/hls/${STREAM_KEY}/index.m3u8" | grep -q 'EXT-X-MAP:URI="init.mp4"'
echo "[PASS] HLS uses fMP4 init.mp4 + m4s"

python3 "${ROOT_DIR}/python/smoke_webrtc_hevc_sdp.py" \
  --base-url "http://${HOST}:${COMPAT_PORT}" \
  --whep-stream-key "${STREAM_KEY}" \
  --whip-stream-key "live/hevc-whip-negotiate-$$"

python3 "${ROOT_DIR}/python/smoke_webrtc_native.py" \
  --base-url "http://${HOST}:${COMPAT_PORT}" \
  --stream-key "${STREAM_KEY}" --duration 6 --play-only

echo "[OTTS] HEVC full-protocol smoke OK: ${STREAM_KEY}"
