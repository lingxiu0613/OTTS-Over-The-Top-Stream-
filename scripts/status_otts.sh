#!/usr/bin/env bash
set -euo pipefail

echo "[OTTS] health:"
curl -s http://127.0.0.1:3000/api/health || true
echo
echo "[OTTS] streams:"
curl -s http://127.0.0.1:8080/api/streams || true
echo
echo "[OTTS] protocol sessions:"
curl -s http://127.0.0.1:8080/api/sessions || true
echo
echo "[OTTS] srt sessions:"
curl -s http://127.0.0.1:3000/api/srt/sessions || true
echo
echo "[OTTS] listeners:"
ss -ltnp | grep -E ':1935|:1985|:3000|:3443|:8080|:8081|:8554|:8556' || true
ss -lunp | grep -E ':9000|:10000' || true
echo
echo "[OTTS] recent core log:"
tail -n 40 /tmp/otts_clean2.out || true
