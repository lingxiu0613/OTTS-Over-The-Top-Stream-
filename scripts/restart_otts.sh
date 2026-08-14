#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

echo "[OTTS] stopping old processes..."
pkill -9 -f build/otts_rtmp 2>/dev/null || true
pkill -9 -f 'node .*src/server.js' 2>/dev/null || true
pkill -9 -f 'python3 .*webrtc_gateway.py' 2>/dev/null || true
fuser -k 1935/tcp 1985/tcp 3000/tcp 3443/tcp 8080/tcp 8081/tcp 8554/tcp 8556/tcp 9000/udp 10000/udp 2>/dev/null || true
sleep 2

echo "[OTTS] starting latest build..."
nohup ./build/otts_rtmp >/tmp/otts_clean2.out 2>/tmp/otts_clean2.err < /dev/null &
CORE_PID=$!
echo "${CORE_PID}" >/tmp/otts_core.pid
sleep 6

echo "[OTTS] core pid: ${CORE_PID}"
echo "[OTTS] health:"
curl -s http://127.0.0.1:3000/api/health || true
echo
echo "[OTTS] streams:"
curl -s http://127.0.0.1:8080/api/streams || true
echo
echo "[OTTS] srt sessions:"
curl -s http://127.0.0.1:3000/api/srt/sessions || true
echo
echo "[OTTS] listeners:"
ss -ltnp | grep -E ':1935|:1985|:3000|:3443|:8080|:8081|:8554|:8556' || true
ss -lunp | grep -E ':9000|:10000' || true
echo
echo "[OTTS] logs:"
tail -n 40 /tmp/otts_clean2.out || true
