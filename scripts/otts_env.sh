#!/usr/bin/env bash
# Shared OTTS environment loader. Existing exported variables win over config file values.

set -euo pipefail

OTTS_ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OTTS_CONFIG_FILE="${OTTS_CONFIG_FILE:-${OTTS_ROOT_DIR}/config/otts.env}"
OTTS_CONFIG_JSON="${OTTS_CONFIG_JSON:-${OTTS_ROOT_DIR}/config/otts.config.json}"

otts_trim() {
  local value="$1"
  value="${value#${value%%[![:space:]]*}}"
  value="${value%${value##*[![:space:]]}}"
  printf '%s' "${value}"
}

otts_load_config_defaults() {
  [[ -f "${OTTS_CONFIG_FILE}" ]] || return 0
  local line key value first last
  while IFS= read -r line || [[ -n "${line}" ]]; do
    line="$(otts_trim "${line}")"
    [[ -z "${line}" || "${line}" == \#* || "${line}" != *=* ]] && continue
    key="$(otts_trim "${line%%=*}")"
    value="$(otts_trim "${line#*=}")"
    first="${value:0:1}"
    last="${value: -1}"
    if [[ ${#value} -ge 2 && (( "${first}" == '"' && "${last}" == '"' ) || ( "${first}" == "'" && "${last}" == "'" )) ]]; then
      value="${value:1:${#value}-2}"
    fi
    if [[ "${key}" =~ ^[A-Za-z_][A-Za-z0-9_]*$ && -z "${!key+x}" ]]; then
      export "${key}=${value}"
    fi
  done < "${OTTS_CONFIG_FILE}"
}

otts_load_env() {
  otts_load_config_defaults

  export OTTS_PUBLIC_HOST="${OTTS_PUBLIC_HOST:-192.168.40.11}"
  export OTTS_RTSP_PUBLIC_HOST="${OTTS_RTSP_PUBLIC_HOST:-${OTTS_PUBLIC_HOST}}"
  export OTTS_SRT_PUBLIC_HOST="${OTTS_SRT_PUBLIC_HOST:-${OTTS_PUBLIC_HOST}}"
  export OTTS_RTMP_PORT="${OTTS_RTMP_PORT:-1935}"
  export OTTS_HTTP_API_PORT="${OTTS_HTTP_API_PORT:-8080}"
  export OTTS_COMPAT_HTTP_PORT="${OTTS_COMPAT_HTTP_PORT:-1985}"
  export PORT="${PORT:-3000}"
  export HTTPS_PORT="${HTTPS_PORT:-3443}"
  export OTTS_WEBRTC_GATEWAY_PORT="${OTTS_WEBRTC_GATEWAY_PORT:-8081}"
  export OTTS_WEBRTC_MODE="${OTTS_WEBRTC_MODE:-auto}"
  export OTTS_LIBWEBRTC_ROOT="${OTTS_LIBWEBRTC_ROOT:-${OTTS_ROOT_DIR}/third_party/libwebrtc-bin/extracted}"
  export OTTS_LIBDATACHANNEL_ROOT="${OTTS_LIBDATACHANNEL_ROOT:-${OTTS_ROOT_DIR}/third_party/libdatachannel-install}"
  export OTTS_RTSP_PUBLISH_PORT="${OTTS_RTSP_PUBLISH_PORT:-8554}"
  export OTTS_RTSP_PLAY_PORT="${OTTS_RTSP_PLAY_PORT:-8556}"
  export OTTS_RTSP_PLAY_COMPAT_ENABLED="${OTTS_RTSP_PLAY_COMPAT_ENABLED:-false}"
  export OTTS_CPP_RTSP_PLAY_PORT="${OTTS_CPP_RTSP_PLAY_PORT:-${OTTS_RTSP_PLAY_PORT}}"
  export OTTS_SRT_PUBLISH_PORT_BASE="${OTTS_SRT_PUBLISH_PORT_BASE:-9000}"
  export OTTS_SRT_PLAY_PORT_BASE="${OTTS_SRT_PLAY_PORT_BASE:-10000}"
  export OTTS_CPP_SRT_PUBLISH_PORT="${OTTS_CPP_SRT_PUBLISH_PORT:-${OTTS_SRT_PUBLISH_PORT_BASE}}"
  export OTTS_CPP_SRT_PLAY_PORT="${OTTS_CPP_SRT_PLAY_PORT:-${OTTS_SRT_PLAY_PORT_BASE}}"
  export OTTS_CPP_SRT_STREAM_KEY="${OTTS_CPP_SRT_STREAM_KEY:-live/srt-demo}"
  export OTTS_SRT_BOOTSTRAP_ENABLED="${OTTS_SRT_BOOTSTRAP_ENABLED:-false}"
  export OTTS_NATIVE_PROTOCOL_ONLY="${OTTS_NATIVE_PROTOCOL_ONLY:-true}"
  export OTTS_API_BASE="${OTTS_API_BASE:-http://127.0.0.1:${OTTS_HTTP_API_PORT}}"
  export OTTS_WEBRTC_GATEWAY_BASE="${OTTS_WEBRTC_GATEWAY_BASE:-http://127.0.0.1:${OTTS_WEBRTC_GATEWAY_PORT}}"
  export OTTS_RTMP_BASE="${OTTS_RTMP_BASE:-rtmp://127.0.0.1:${OTTS_RTMP_PORT}}"
  export OTTS_AUTH_TTL_SECONDS="${OTTS_AUTH_TTL_SECONDS:-3600}"
  export OTTS_FFMPEG_BIN="${OTTS_FFMPEG_BIN:-ffmpeg}"
  export OTTS_RECORDING_ROOT="${OTTS_RECORDING_ROOT:-/tmp/otts_recordings}"
  export OTTS_CONFIG_JSON="${OTTS_CONFIG_JSON}"
  export OTTS_CLEANUP_INTERVAL_MS="${OTTS_CLEANUP_INTERVAL_MS:-5000}"
  # Allow short source/network stalls without deleting the live stream object.
  export OTTS_EXTERNAL_PUBLISHER_IDLE_MS="${OTTS_EXTERNAL_PUBLISHER_IDLE_MS:-120000}"
  export OTTS_STOPPED_SESSION_RETENTION_MS="${OTTS_STOPPED_SESSION_RETENTION_MS:-60000}"
}

otts_tcp_ports() {
  {
    printf '%s
'       "${OTTS_RTMP_PORT}"       "${OTTS_COMPAT_HTTP_PORT}"       "${PORT}"       "${HTTPS_PORT}"       "${OTTS_HTTP_API_PORT}"       "${OTTS_WEBRTC_GATEWAY_PORT}"       "${OTTS_RTSP_PUBLISH_PORT}"
    if [[ "${OTTS_RTSP_PLAY_COMPAT_ENABLED:-false}" != "false" ]]; then
      printf '%s
' "${OTTS_RTSP_PLAY_PORT}"
    fi
    if [[ "${OTTS_CPP_RTSP_PLAY_PORT:-0}" != "0" ]]; then
      printf '%s
' "${OTTS_CPP_RTSP_PLAY_PORT}"
    fi
  } | awk 'NF && !seen[$0]++'
}
otts_udp_ports() {
  printf '%s\n' \
    "${OTTS_SRT_PUBLISH_PORT_BASE}" \
    "${OTTS_SRT_PLAY_PORT_BASE}" \
    "${OTTS_CPP_SRT_PUBLISH_PORT}" \
    "${OTTS_CPP_SRT_PLAY_PORT}" | awk 'NF && !seen[$0]++'
}
