#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${ROOT_DIR}/scripts/otts_env.sh"
otts_load_env

echo "[OTTS] config: ${OTTS_CONFIG_FILE}"
echo "[OTTS] health:"
curl -s "http://127.0.0.1:${PORT}/api/health" || true
echo
echo "[OTTS] streams:"
curl -s "http://127.0.0.1:${OTTS_HTTP_API_PORT}/api/streams" || true
echo
echo "[OTTS] protocol sessions:"
curl -s "http://127.0.0.1:${PORT}/api/protocol/sessions" || true
echo
echo "[OTTS] srt sessions:"
curl -s "http://127.0.0.1:${PORT}/api/srt/sessions" || true
echo
echo "[OTTS] listeners:"
ss -ltnp | grep -E ":(${OTTS_RTMP_PORT}|${OTTS_COMPAT_HTTP_PORT}|${PORT}|${HTTPS_PORT}|${OTTS_HTTP_API_PORT}|${OTTS_WEBRTC_GATEWAY_PORT}|${OTTS_RTSP_PUBLISH_PORT}|${OTTS_RTSP_PLAY_PORT}|${OTTS_CPP_RTSP_PLAY_PORT})" || true
ss -lunp | grep -E ":(${OTTS_SRT_PUBLISH_PORT_BASE}|${OTTS_SRT_PLAY_PORT_BASE})" || true
echo
echo "[OTTS] recent core log:"
tail -n 40 /tmp/otts_clean2.out || true
