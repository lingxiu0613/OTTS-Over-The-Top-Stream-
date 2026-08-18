#!/usr/bin/env bash
set -euo pipefail

STREAM_KEY="${1:-live/hls-smoke}"
RTMP_URL="rtmp://127.0.0.1:1935/${STREAM_KEY}"
HLS_URL="http://127.0.0.1:3000/hls/${STREAM_KEY}/index.m3u8"
API_BASE="${OTTS_API_BASE:-http://127.0.0.1:8080}"
NODE_BASE="${OTTS_NODE_BASE:-http://127.0.0.1:3000}"

echo "[OTTS] HLS smoke test on ${STREAM_KEY}"
echo "[OTTS] publish: ${RTMP_URL}"
echo "[OTTS] hls: ${HLS_URL}"

ffmpeg -hide_banner -loglevel warning \
  -re \
  -f lavfi -i testsrc=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100 \
  -t 24 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p -g 50 -keyint_min 50 \
  -c:a aac \
  -f flv "${RTMP_URL}" \
  >/tmp/otts_hls_push_test.out 2>/tmp/otts_hls_push_test.err &
PUSH_PID=$!

cleanup() {
  curl -fsS -X POST "${NODE_BASE}/api/streams/hls/stop?stream_key=$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "${STREAM_KEY}")" >/dev/null 2>&1 || true
  kill "${PUSH_PID}" 2>/dev/null || true
  wait "${PUSH_PID}" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 30); do
  if curl -fsS "${API_BASE}/api/streams" | grep -q '"ready_for_play":true'; then
    break
  fi
  sleep 0.5
done

ENCODED_KEY="$(python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=""))' "${STREAM_KEY}")"
echo "[OTTS] start HLS"
curl -fsS -X POST "${NODE_BASE}/api/streams/hls/start?stream_key=${ENCODED_KEY}" >/tmp/otts_hls_start.json
cat /tmp/otts_hls_start.json
echo

for _ in $(seq 1 40); do
  if curl -fsS "${HLS_URL}" 2>/dev/null | grep -q '#EXTINF'; then
    break
  fi
  sleep 0.5
done

echo "[OTTS] playlist:"
curl -fsS "${HLS_URL}" | head -n 20
curl -fsS "${HLS_URL}" | grep -q '#EXTINF'

echo "[OTTS] ffprobe HLS play check:"
timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "${HLS_URL}" > /tmp/otts_hls_probe.out 2>/tmp/otts_hls_probe.err || rc=$?
rc="${rc:-0}"
if [[ "${rc}" != "0" && "${rc}" != "124" ]]; then
  cat /tmp/otts_hls_probe.err >&2 || true
  exit "${rc}"
fi
if [[ ! -s /tmp/otts_hls_probe.out ]]; then
  cat /tmp/otts_hls_probe.err >&2 || true
  echo "[OTTS] no HLS stream info received" >&2
  exit 1
fi
head -n 20 /tmp/otts_hls_probe.out
grep -Eq 'h264|aac|opus|codec_type=' /tmp/otts_hls_probe.out

echo "[OTTS] HLS status:"
curl -fsS "${NODE_BASE}/api/streams/hls/status?stream_key=${ENCODED_KEY}"
echo
