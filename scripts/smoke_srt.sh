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

echo "[OTTS] SRT native session state:"
curl -fsS http://127.0.0.1:8080/api/sessions | grep -q '"source_protocol":"srt"'
curl -fsS http://127.0.0.1:8080/api/sessions
echo

echo "[OTTS] ffprobe play check:"
SRT_PROBE_OUT=/tmp/otts_srt_probe.out
SRT_PROBE_ERR=/tmp/otts_srt_probe.err
set +e
timeout 12s ffprobe -hide_banner -loglevel error -show_streams -of compact=p=0:nk=1 "${PLAY_URL}" >"${SRT_PROBE_OUT}" 2>"${SRT_PROBE_ERR}"
rc=$?
set -e
if [[ "${rc}" != "0" && "${rc}" != "124" ]]; then
  cat "${SRT_PROBE_ERR}" >&2 || true
  exit "${rc}"
fi
if [[ ! -s "${SRT_PROBE_OUT}" ]]; then
  cat "${SRT_PROBE_ERR}" >&2 || true
  echo "[OTTS] no SRT stream info received" >&2
  exit 1
fi
head -n 20 "${SRT_PROBE_OUT}"
grep -Eq 'h264|aac|opus|codec_type=' "${SRT_PROBE_OUT}"
echo

kill "${PUSH_PID}" 2>/dev/null || true
wait "${PUSH_PID}" 2>/dev/null || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_srt_push_test.err || true
