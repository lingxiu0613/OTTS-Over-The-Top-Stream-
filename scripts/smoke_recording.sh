#!/usr/bin/env bash
set -euo pipefail

STREAM_KEY="${1:-live/recording-smoke}"
RTMP_URL="rtmp://127.0.0.1:1935/${STREAM_KEY}"
API_BASE="${OTTS_API_BASE:-http://127.0.0.1:8080}"
NODE_BASE="${OTTS_NODE_BASE:-http://127.0.0.1:3000}"
ENCODED_KEY="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "${STREAM_KEY}")"

echo "[OTTS] recording smoke test on ${STREAM_KEY}"
echo "[OTTS] publish: ${RTMP_URL}"

ffmpeg -hide_banner -loglevel warning \
  -re \
  -f lavfi -i testsrc=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100 \
  -t 36 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 50 -keyint_min 50 \
  -c:a aac \
  -f flv "${RTMP_URL}" \
  >/tmp/otts_recording_push_test.out 2>/tmp/otts_recording_push_test.err &
PUSH_PID=$!

cleanup() {
  local rc=$?
  curl -fsS -X POST "${NODE_BASE}/api/recordings/stop?stream_key=${ENCODED_KEY}" >/dev/null 2>&1 || true
  kill "${PUSH_PID}" 2>/dev/null || true
  wait "${PUSH_PID}" 2>/dev/null || true
  exit "${rc}"
}
trap cleanup EXIT

for _ in $(seq 1 30); do
  if curl -fsS "${API_BASE}/api/streams" | grep -q '"ready_for_play":true'; then
    break
  fi
  sleep 0.5
done

run_recording_case() {
  local format="$1"
  echo "[OTTS] start ${format} recording"
  curl -fsS -X POST "${NODE_BASE}/api/recordings/start?stream_key=${ENCODED_KEY}&format=${format}" >/tmp/otts_recording_start.json
  cat /tmp/otts_recording_start.json
  echo
  sleep 5
  echo "[OTTS] stop ${format} recording"
  curl -fsS -X POST "${NODE_BASE}/api/recordings/stop?stream_key=${ENCODED_KEY}" >/tmp/otts_recording_stop.json
  cat /tmp/otts_recording_stop.json
  echo
  jq -e '.ok == true and (.recording.bytes // 0) > 0 and (.recording.download_ready == true)' /tmp/otts_recording_stop.json >/dev/null
}

run_recording_case flv
run_recording_case mp4

echo "[OTTS] recording files:"
curl -fsS "${NODE_BASE}/api/recordings" >/tmp/otts_recording_files.json
jq --arg stream "${STREAM_KEY}" '[.files[] | select(.stream_key == $stream)] | length >= 2' /tmp/otts_recording_files.json | grep -q true
jq --arg stream "${STREAM_KEY}" '.files[] | select(.stream_key == $stream) | {file_name, format, bytes, download_ready}' /tmp/otts_recording_files.json
