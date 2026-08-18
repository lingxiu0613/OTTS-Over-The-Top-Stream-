#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT_DIR}/scripts/otts_smoke_lib.sh"

STREAM_KEY="${1:-live/cleanup-smoke}"
RTMP_URL="$(otts_rtmp_publish_url "${STREAM_KEY}")"
FLV_URL="$(otts_append_play_auth "http://127.0.0.1:${OTTS_HTTP_API_PORT}/${STREAM_KEY}.flv" "${STREAM_KEY}")"

echo "[OTTS] cleanup smoke test on ${STREAM_KEY}"
echo "[OTTS] publish: ${RTMP_URL}"
echo "[OTTS] play: ${FLV_URL}"

ffmpeg -hide_banner -loglevel warning \
  -re \
  -f lavfi -i testsrc=size=640x360:rate=25 \
  -f lavfi -i sine=frequency=1000:sample_rate=44100 \
  -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
  -c:a aac \
  -shortest -t 6 \
  -f flv "${RTMP_URL}" \
  >/tmp/otts_cleanup_push.out 2>/tmp/otts_cleanup_push.err &
PUSH_PID=$!

for _ in $(seq 1 20); do
  if curl -fsS "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/streams" | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
    break
  fi
  sleep 0.3
done

timeout 5s curl -fsS "${FLV_URL}" >/tmp/otts_cleanup_play.flv 2>/tmp/otts_cleanup_play.err || true
wait "${PUSH_PID}" || true

STREAMS_JSON=""
SESSIONS_JSON=""
for _ in $(seq 1 35); do
  curl -fsS -X POST "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/maintenance/cleanup?external_idle_ms=1&stopped_retention_ms=1" \
    >/tmp/otts_cleanup_result.json
  STREAMS_JSON="$(curl -fsS "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/streams")"
  SESSIONS_JSON="$(curl -fsS "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/sessions")"
  if ! printf '%s' "${STREAMS_JSON}" | grep -q "\"stream_key\":\"${STREAM_KEY}\"" &&
     ! printf '%s' "${SESSIONS_JSON}" | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
    break
  fi
  sleep 1
done

if printf '%s' "${STREAMS_JSON}" | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
  echo "[FAIL] stream still present after cleanup"
  printf '%s\n' "${STREAMS_JSON}"
  exit 1
fi

if printf '%s' "${SESSIONS_JSON}" | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
  echo "[FAIL] protocol session still present after cleanup"
  printf '%s\n' "${SESSIONS_JSON}"
  exit 1
fi

echo "[PASS] cleanup removed inactive stream/session state"
cat /tmp/otts_cleanup_result.json
echo
