#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "${ROOT_DIR}/scripts/otts_env.sh"
otts_load_env
cd "${ROOT_DIR}"

STRICT=false
[[ "${1:-}" == "--strict" ]] && STRICT=true
FAIL=0

check_cmd() {
  if command -v "$1" >/dev/null 2>&1; then
    echo "[OK] command $1: $(command -v "$1")"
  else
    echo "[FAIL] missing command: $1"
    FAIL=1
  fi
}

check_file() {
  if [[ -e "$1" ]]; then
    echo "[OK] file $1"
  else
    echo "[FAIL] missing file: $1"
    FAIL=1
  fi
}

check_tcp_port() {
  local port="$1"
  local owners
  owners="$(ss -ltnp 2>/dev/null | awk -v p=":${port}" '$4 ~ p"$" {print}')"
  if [[ -z "${owners}" ]]; then
    echo "[OK] tcp/${port}: free"
    return
  fi
  if grep -Eq 'otts_rtmp|node|python3' <<<"${owners}"; then
    echo "[WARN] tcp/${port}: used by OTTS process"
    echo "${owners}" | sed 's/^/       /'
  else
    echo "[FAIL] tcp/${port}: occupied by another process"
    echo "${owners}" | sed 's/^/       /'
    FAIL=1
  fi
}

check_udp_port() {
  local port="$1"
  local owners
  owners="$(ss -lunp 2>/dev/null | awk -v p=":${port}" '$4 ~ p"$" {print}')"
  if [[ -z "${owners}" ]]; then
    echo "[OK] udp/${port}: free"
    return
  fi
  if grep -Eq 'ffmpeg|otts_rtmp|node|python3' <<<"${owners}"; then
    echo "[WARN] udp/${port}: used by OTTS process"
    echo "${owners}" | sed 's/^/       /'
  else
    echo "[FAIL] udp/${port}: occupied by another process"
    echo "${owners}" | sed 's/^/       /'
    FAIL=1
  fi
}

echo "[OTTS] preflight config: ${OTTS_CONFIG_FILE}"
check_file build/otts_rtmp
check_cmd ffmpeg
check_cmd ffprobe
check_cmd curl
check_cmd ss
check_cmd fuser
check_cmd node
check_cmd python3

echo "[OTTS] configured TCP ports"
while IFS= read -r port; do check_tcp_port "${port}"; done < <(otts_tcp_ports)

echo "[OTTS] configured UDP ports"
while IFS= read -r port; do check_udp_port "${port}"; done < <(otts_udp_ports)

if [[ "${STRICT}" == "true" && "${FAIL}" -ne 0 ]]; then
  echo "[OTTS] preflight failed"
  exit 1
fi
if [[ "${FAIL}" -ne 0 ]]; then
  echo "[OTTS] preflight completed with warnings/failures"
else
  echo "[OTTS] preflight OK"
fi
exit 0
