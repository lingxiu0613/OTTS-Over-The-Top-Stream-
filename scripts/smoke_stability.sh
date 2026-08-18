#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT_DIR}/scripts/otts_smoke_lib.sh"

STREAM_KEY="${1:-live/stability-smoke}"
API_BASE="http://127.0.0.1:${OTTS_HTTP_API_PORT}"
RTMP_URL="$(otts_rtmp_publish_url "${STREAM_KEY}")"
FLV_URL="$(otts_append_play_auth "${API_BASE}/${STREAM_KEY}.flv" "${STREAM_KEY}")"

cleanup() {
  for pid in "${PUBLISH_A_PID:-}" "${PUBLISH_B_PID:-}" "${PLAY_PID:-}"; do
    [[ -n "${pid}" && "${pid}" != "0" ]] || continue
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
  done
  curl -fsS -X POST "${API_BASE}/api/streams/disconnect?stream_key=${STREAM_KEY}" >/dev/null 2>&1 || true
  curl -fsS -X POST "${API_BASE}/api/maintenance/cleanup?external_idle_ms=1&stopped_retention_ms=1" >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_stream_present() {
  for _ in $(seq 1 30); do
    if curl -fsS "${API_BASE}/api/streams" | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
      return 0
    fi
    sleep 0.3
  done
  echo "[FAIL] stream did not appear: ${STREAM_KEY}"
  curl -fsS "${API_BASE}/api/streams" || true
  exit 1
}

wait_stream_absent() {
  for _ in $(seq 1 40); do
    curl -fsS -X POST "${API_BASE}/api/maintenance/cleanup?external_idle_ms=1&stopped_retention_ms=1" >/tmp/otts_stability_cleanup.json
    if ! curl -fsS "${API_BASE}/api/streams" | grep -q "\"stream_key\":\"${STREAM_KEY}\""; then
      return 0
    fi
    sleep 1
  done
  echo "[FAIL] stream still present after cleanup wait"
  curl -fsS "${API_BASE}/api/streams" || true
  exit 1
}

start_publish() {
  local label="$1"
  local duration="$2"
  ffmpeg -hide_banner -loglevel warning \
    -re \
    -f lavfi -i "testsrc=size=640x360:rate=25" \
    -f lavfi -i "sine=frequency=$((1000 + duration * 10)):sample_rate=44100" \
    -c:v libx264 -preset veryfast -tune zerolatency -pix_fmt yuv420p \
    -c:a aac \
    -shortest -t "${duration}" \
    -f flv "${RTMP_URL}" \
    >"/tmp/otts_stability_${label}.out" 2>"/tmp/otts_stability_${label}.err" &
  echo $!
}

echo "[OTTS] stability smoke test on ${STREAM_KEY}"
echo "[OTTS] publish: ${RTMP_URL}"
echo "[OTTS] play: ${FLV_URL}"

PUBLISH_A_PID="$(start_publish publish_a 20)"
wait_stream_present

timeout 4s curl -fsS "${FLV_URL}" >/tmp/otts_stability_play.flv 2>/tmp/otts_stability_play.err &
PLAY_PID=$!
sleep 1
kill "${PLAY_PID}" 2>/dev/null || true
wait "${PLAY_PID}" 2>/dev/null || true
echo "[OK] early player disconnect handled"

PUBLISH_B_PID="$(start_publish publish_b 6)"
sleep 2
if kill -0 "${PUBLISH_A_PID}" 2>/dev/null; then
  echo "[FAIL] duplicate publisher did not replace old publisher"
  exit 1
fi
echo "[OK] duplicate publisher replacement handled"

for _ in $(seq 1 20); do
  if ! kill -0 "${PUBLISH_B_PID}" 2>/dev/null; then
    break
  fi
  sleep 0.5
done
kill "${PUBLISH_B_PID}" 2>/dev/null || true
PUBLISH_B_PID=0
wait_stream_absent
echo "[OK] stream state cleaned after publisher stop"

PUBLISH_A_PID=0
echo "[PASS] stability smoke completed"
