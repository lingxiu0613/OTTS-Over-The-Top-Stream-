#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT_DIR}/scripts/otts_smoke_lib.sh"
otts_load_env

INPUT_FILE="$(otts_sample_file "${1:-}")" || { echo "[OTTS] missing input file" >&2; exit 1; }
STREAM_KEY="${2:-live/cpp-rtsp-smoke}"
CPP_RTSP_PORT="${OTTS_CPP_RTSP_PLAY_PORT:-8560}"
if [[ "${CPP_RTSP_PORT}" == "0" ]]; then
  echo "[OTTS] OTTS_CPP_RTSP_PLAY_PORT is disabled" >&2
  exit 1
fi

PUBLISH_URL="$(otts_rtmp_publish_url "${STREAM_KEY}")"
MOUNT="${STREAM_KEY//\//__}.sdp"
if [[ -n "${OTTS_STREAM_TOKEN:-}" ]]; then
  PLAY_URL="rtsp://127.0.0.1:${CPP_RTSP_PORT}/${MOUNT}?token=${OTTS_STREAM_TOKEN}"
elif [[ -n "${OTTS_AUTH_SECRET:-}" ]]; then
  PLAY_URL="$(OTTS_AUTH_SECRET="${OTTS_AUTH_SECRET}" "${SMOKE_DIR}/sign_stream_url.py" "${STREAM_KEY}" --ttl "${OTTS_AUTH_TTL_SECONDS:-3600}" --host 127.0.0.1 --rtsp-play-port "${CPP_RTSP_PORT}"     | awk -F': ' '$1 == "rtsp_play" { print $2 }')"
else
  PLAY_URL="rtsp://127.0.0.1:${CPP_RTSP_PORT}/${MOUNT}"
fi

echo "[OTTS] C++ RTSP play smoke test"
echo "[OTTS] input: ${INPUT_FILE}"
echo "[OTTS] stream: ${STREAM_KEY}"
echo "[OTTS] publish: ${PUBLISH_URL}"
echo "[OTTS] play: ${PLAY_URL}"

ffmpeg -hide_banner -loglevel warning   -re -stream_loop -1 -i "${INPUT_FILE}"   -c copy -f flv "${PUBLISH_URL}"   >/tmp/otts_cpp_rtsp_push_test.out 2>/tmp/otts_cpp_rtsp_push_test.err &
PUSH_PID=$!

sleep 5

echo "[OTTS] ffprobe C++ RTSP play check:"
PROBE_OUTPUT="$(timeout 12s ffprobe -hide_banner -loglevel error -rtsp_transport udp   -show_entries stream=codec_name,codec_type -of csv=p=0 "${PLAY_URL}" || true)"
printf '%s
' "${PROBE_OUTPUT}" | head -n 20
if grep -q '^h264,video' <<<"${PROBE_OUTPUT}" && grep -q '^aac,audio' <<<"${PROBE_OUTPUT}"; then
  PROBE_OK=true
else
  PROBE_OK=false
fi
echo

kill "${PUSH_PID}" 2>/dev/null || true
wait "${PUSH_PID}" 2>/dev/null || true

echo "[OTTS] push stderr:"
tail -n 30 /tmp/otts_cpp_rtsp_push_test.err || true

[[ "${PROBE_OK}" == "true" ]]
