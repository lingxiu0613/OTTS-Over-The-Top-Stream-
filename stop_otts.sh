#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${ROOT_DIR}/scripts/otts_env.sh"
otts_load_env

pkill -9 -f build/otts_rtmp 2>/dev/null || true
pkill -9 -f 'node .*src/server.js' 2>/dev/null || true
pkill -9 -f 'python3 .*webrtc_gateway.py' 2>/dev/null || true
for port in $(otts_tcp_ports); do
  fuser -k "${port}/tcp" 2>/dev/null || true
done
for port in $(otts_udp_ports); do
  fuser -k "${port}/udp" 2>/dev/null || true
done
sleep 1
echo "[OTTS] stopped."
