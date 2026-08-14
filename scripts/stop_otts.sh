#!/usr/bin/env bash
set -euo pipefail

pkill -9 -f build/otts_rtmp 2>/dev/null || true
pkill -9 -f 'node .*src/server.js' 2>/dev/null || true
pkill -9 -f 'python3 .*webrtc_gateway.py' 2>/dev/null || true
fuser -k 1935/tcp 1985/tcp 3000/tcp 3443/tcp 8080/tcp 8081/tcp 8554/tcp 8556/tcp 9000/udp 10000/udp 2>/dev/null || true
sleep 1
echo "[OTTS] stopped."
