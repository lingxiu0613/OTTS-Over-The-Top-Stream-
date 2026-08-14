#!/usr/bin/env bash
set -euo pipefail

INPUT_FILE="${1:-news_1280x720.mp4}"
PUBLISH_URL="${2:-rtsp://127.0.0.1:8554/live__rtsp-compat.sdp}"
PLAY_URL="${3:-rtsp://127.0.0.1:8556/live__rtsp-compat.sdp}"

echo "[OTTS] RTSP smoke test"
echo "[OTTS] input: ${INPUT_FILE}"
echo "[OTTS] publish: ${PUBLISH_URL}"
echo "[OTTS] play: ${PLAY_URL}"

ffmpeg -hide_banner -loglevel warning \
  -re -i "${INPUT_FILE}" \
  -rtsp_transport udp \
  -vcodec h264 \
  -f rtsp "${PUBLISH_URL}" \
  >/tmp/otts_rtsp_push_test.out 2>/tmp/otts_rtsp_push_test.err &
PUSH_PID=$!

sleep 4

echo "[OTTS] ffprobe play check:"
timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "${PLAY_URL}" | head -n 20 || true
echo

kill "${PUSH_PID}" 2>/dev/null || true
wait "${PUSH_PID}" 2>/dev/null || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_rtsp_push_test.err || true
