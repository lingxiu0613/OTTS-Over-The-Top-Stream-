#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUT_FILE="${1:-${ROOT_DIR}/news_1280x720.mp4}"
if [[ "${INPUT_FILE}" != /* ]]; then
  INPUT_FILE="${ROOT_DIR}/${INPUT_FILE}"
fi

echo "[OTTS] native protocol smoke suite"
echo "[OTTS] input: ${INPUT_FILE}"

"${ROOT_DIR}/scripts/smoke_rtmp.sh" live/rtmp-smoke
echo

"${ROOT_DIR}/scripts/smoke_rtsp.sh" "${INPUT_FILE}" \
  rtsp://127.0.0.1:8554/live__rtsp-native.sdp \
  rtsp://127.0.0.1:8556/live__rtsp-native.sdp
echo

"${ROOT_DIR}/scripts/smoke_srt.sh" "${INPUT_FILE}" \
  "srt://127.0.0.1:9000?mode=caller&transtype=live" \
  "srt://127.0.0.1:10000?mode=caller&transtype=live"
echo

python3 "${ROOT_DIR}/python/smoke_webrtc_native.py" \
  --base-url http://127.0.0.1:1985 \
  --stream-key live/webrtc-smoke
echo

"${ROOT_DIR}/scripts/smoke_cleanup.sh" live/cleanup-smoke

echo "[OTTS] native protocol smoke suite finished"
