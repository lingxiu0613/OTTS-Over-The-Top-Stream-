#!/usr/bin/env bash
set -euo pipefail

INPUT_FILE="${1:-news_1280x720.mp4}"
PUBLISH_URL="${2:-srt://127.0.0.1:9000?mode=caller&transtype=live}"
PLAY_URL="${3:-srt://127.0.0.1:10000?mode=caller&transtype=live}"

echo "[OTTS] SRT smoke test"
echo "[OTTS] input: ${INPUT_FILE}"
echo "[OTTS] publish: ${PUBLISH_URL}"
echo "[OTTS] play: ${PLAY_URL}"

if [[ -f "${INPUT_FILE}" ]]; then
  ffmpeg -hide_banner -loglevel warning \
    -re -i "${INPUT_FILE}" \
    -c copy -f mpegts "${PUBLISH_URL}" \
    >/tmp/otts_srt_push_test.out 2>/tmp/otts_srt_push_test.err &
else
  echo "[OTTS] input file not found; using generated SRT test source"
  ffmpeg -hide_banner -loglevel warning \
    -re \
    -f lavfi -i testsrc=size=640x360:rate=25 \
    -f lavfi -i sine=frequency=1000:sample_rate=48000 \
    -t 14 \
    -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
    -c:a aac \
    -f mpegts "${PUBLISH_URL}" \
    >/tmp/otts_srt_push_test.out 2>/tmp/otts_srt_push_test.err &
fi
PUSH_PID=$!

sleep 4

echo "[OTTS] ffprobe play check:"
timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "${PLAY_URL}" | head -n 20 || true
echo

kill "${PUSH_PID}" 2>/dev/null || true
wait "${PUSH_PID}" 2>/dev/null || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_srt_push_test.err || true
