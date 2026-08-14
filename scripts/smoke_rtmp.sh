#!/usr/bin/env bash
set -euo pipefail

STREAM_KEY="${1:-live/rtmp-smoke}"

echo "[OTTS] RTMP smoke test on ${STREAM_KEY}"

ffmpeg -hide_banner -loglevel warning \
  -re \
  -f lavfi -i testsrc=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac \
  -shortest -t 8 \
  -f flv "rtmp://127.0.0.1:1935/${STREAM_KEY}" \
  >/tmp/otts_rtmp_push_test.out 2>/tmp/otts_rtmp_push_test.err &
PUSH_PID=$!

sleep 3

echo "[OTTS] stream state during publish:"
curl -s http://127.0.0.1:8080/api/streams || true
echo

echo "[OTTS] ffprobe play check:"
timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "rtmp://127.0.0.1:1935/${STREAM_KEY}" | head -n 20 || true
echo

wait "${PUSH_PID}" || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_rtmp_push_test.err || true
