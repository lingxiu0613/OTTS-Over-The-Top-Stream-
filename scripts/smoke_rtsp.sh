#!/usr/bin/env bash
set -euo pipefail

INPUT_FILE="${1:-news_1280x720.mp4}"
PUBLISH_URL="${2:-rtsp://127.0.0.1:8554/live__rtsp-native.sdp}"
PLAY_URL="${3:-rtsp://127.0.0.1:8556/live__rtsp-native.sdp}"

echo "[OTTS] native RTSP smoke test"
echo "[OTTS] input: ${INPUT_FILE}"
echo "[OTTS] publish: ${PUBLISH_URL}"
echo "[OTTS] play: ${PLAY_URL}"

STREAM_PATH="${PLAY_URL#rtsp://*/}"
STREAM_PATH="${STREAM_PATH%%\?*}"
STREAM_KEY="${STREAM_PATH%.sdp}"
STREAM_KEY="${STREAM_KEY//__/\/}"

if [[ -f "${INPUT_FILE}" ]]; then
  ffmpeg -hide_banner -loglevel warning \
    -re -i "${INPUT_FILE}" \
    -rtsp_transport udp \
    -vcodec h264 \
    -f rtsp "${PUBLISH_URL}" \
    >/tmp/otts_rtsp_push_test.out 2>/tmp/otts_rtsp_push_test.err &
else
  echo "[OTTS] input file not found; using generated RTSP test source"
  ffmpeg -hide_banner -loglevel warning \
    -re \
    -f lavfi -i testsrc=size=640x360:rate=25 \
    -t 14 \
    -rtsp_transport udp \
    -vcodec h264 \
    -pix_fmt yuv420p \
    -f rtsp "${PUBLISH_URL}" \
    >/tmp/otts_rtsp_push_test.out 2>/tmp/otts_rtsp_push_test.err &
fi
PUSH_PID=$!

for _ in $(seq 1 20); do
  if curl -s http://127.0.0.1:8080/api/streams | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
    break
  fi
  sleep 0.5
done
sleep 2

echo "[OTTS] ffprobe play check:"
timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "${PLAY_URL}" | head -n 20 || true
echo

kill "${PUSH_PID}" 2>/dev/null || true
wait "${PUSH_PID}" 2>/dev/null || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_rtsp_push_test.err || true
