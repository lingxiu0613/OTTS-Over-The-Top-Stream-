#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -f "${ROOT_DIR}/scripts/otts_env.sh" ]]; then
  ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi
source "${ROOT_DIR}/scripts/otts_env.sh"
otts_load_env
cd "${ROOT_DIR}"

echo "[OTTS] config: ${OTTS_CONFIG_FILE}"
echo "[OTTS] preflight:"
"${ROOT_DIR}/scripts/preflight_otts.sh" || true

echo "[OTTS] stopping old processes..."
pkill -9 -f build/otts_rtmp 2>/dev/null || true
pkill -9 -f 'node .*src/server.js' 2>/dev/null || true
pkill -9 -f 'python3 .*webrtc_gateway.py' 2>/dev/null || true
for port in $(otts_tcp_ports); do
  fuser -k "${port}/tcp" 2>/dev/null || true
done
for port in $(otts_udp_ports); do
  fuser -k "${port}/udp" 2>/dev/null || true
done
sleep 2

echo "[OTTS] starting latest build..."
nohup ./build/otts_rtmp >/tmp/otts_clean2.out 2>/tmp/otts_clean2.err < /dev/null &
CORE_PID=$!
echo "${CORE_PID}" >/tmp/otts_core.pid
sleep 6

echo "[OTTS] core pid: ${CORE_PID}"
echo "[OTTS] health:"
curl -s "http://127.0.0.1:${PORT}/api/health" || true
echo
echo "[OTTS] streams:"
curl -s "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/streams" || true
echo
echo "[OTTS] srt sessions:"
curl -s "http://127.0.0.1:${PORT}/api/srt/sessions" || true
echo
echo "[OTTS] listeners:"
ss -ltnp | grep -E ":(${OTTS_RTMP_PORT}|${OTTS_COMPAT_HTTP_PORT}|${PORT}|${HTTPS_PORT}|${OTTS_HTTP_API_PORT}|${OTTS_WEBRTC_GATEWAY_PORT}|${OTTS_RTSP_PUBLISH_PORT}|${OTTS_RTSP_PLAY_PORT})" || true
ss -lunp | grep -E ":(${OTTS_SRT_PUBLISH_PORT_BASE}|${OTTS_SRT_PLAY_PORT_BASE})" || true
echo
echo "[OTTS] logs:"
tail -n 40 /tmp/otts_clean2.out || true
