#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT_DIR}/scripts/otts_smoke_lib.sh"

STREAM_KEY="${1:-live/rtmp-smoke}"
PUBLISH_URL="$(otts_rtmp_publish_url "${STREAM_KEY}")"
PLAY_URL="$(otts_rtmp_play_url "${STREAM_KEY}")"

echo "[OTTS] RTMP smoke test on ${STREAM_KEY}"
echo "[OTTS] publish: ${PUBLISH_URL}"
echo "[OTTS] play: ${PLAY_URL}"

ffmpeg -hide_banner -loglevel warning \
  -re \
  -f lavfi -i testsrc=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac \
  -shortest -t 8 \
  -f flv "${PUBLISH_URL}" \
  >/tmp/otts_rtmp_push_test.out 2>/tmp/otts_rtmp_push_test.err &
PUSH_PID=$!

sleep 3

echo "[OTTS] stream state during publish:"
curl -s "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/streams" || true
echo

echo "[OTTS] ffprobe play check:"
if timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "${PLAY_URL}" | head -n 20; then
  PROBE_OK=true
else
  PROBE_OK=false
fi
echo

wait "${PUSH_PID}" || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_rtmp_push_test.err || true

[[ "${PROBE_OK}" == "true" ]]
