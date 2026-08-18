#!/usr/bin/env bash
set -euo pipefail

SMOKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${SMOKE_DIR}/otts_env.sh"
otts_load_env

otts_sample_file() {
  local candidate
  for candidate in \
    "${1:-}" \
    "./news_1280x720.mp4" \
    "../news_1280x720.mp4" \
    "/home/nolovr/Downloads/news_1280x720.mp4" \
    "/home/nolovr/Downloads/ffmpeg_whip_whep/FFmpeg-WHIP-WHEP/build/news.mp4"; do
    [[ -n "${candidate}" && -f "${candidate}" ]] && { printf '%s' "${candidate}"; return 0; }
  done
  return 1
}

otts_query_join() {
  [[ "$1" == *\?* ]] && printf '&' || printf '?'
}

otts_append_play_auth() {
  local url="$1"
  local stream_key="$2"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" ]]; then
    printf '%s%stoken=%s' "${url}" "$(otts_query_join "${url}")" "${OTTS_STREAM_TOKEN}"
  elif [[ -n "${OTTS_AUTH_SECRET:-}" ]]; then
    OTTS_AUTH_SECRET="${OTTS_AUTH_SECRET}" "${SMOKE_DIR}/sign_stream_url.py" "${stream_key}" --ttl "${OTTS_AUTH_TTL_SECONDS:-3600}" --host 127.0.0.1 \
      | awk -F': ' -v kind="http_flv_play" '$1 == kind { print $2 }'
  else
    printf '%s' "${url}"
  fi
}

otts_rtmp_publish_url() {
  local stream_key="$1"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" ]]; then
    printf 'rtmp://127.0.0.1:%s/%s?token=%s' "${OTTS_RTMP_PORT}" "${stream_key}" "${OTTS_STREAM_TOKEN}"
  elif [[ -n "${OTTS_AUTH_SECRET:-}" ]]; then
    OTTS_AUTH_SECRET="${OTTS_AUTH_SECRET}" "${SMOKE_DIR}/sign_stream_url.py" "${stream_key}" --ttl "${OTTS_AUTH_TTL_SECONDS:-3600}" --host 127.0.0.1 --rtmp-port "${OTTS_RTMP_PORT}" \
      | awk -F': ' '$1 == "rtmp_publish" { print $2 }'
  else
    printf 'rtmp://127.0.0.1:%s/%s' "${OTTS_RTMP_PORT}" "${stream_key}"
  fi
}

otts_rtmp_play_url() {
  local stream_key="$1"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" ]]; then
    printf 'rtmp://127.0.0.1:%s/%s?token=%s' "${OTTS_RTMP_PORT}" "${stream_key}" "${OTTS_STREAM_TOKEN}"
  elif [[ -n "${OTTS_AUTH_SECRET:-}" ]]; then
    OTTS_AUTH_SECRET="${OTTS_AUTH_SECRET}" "${SMOKE_DIR}/sign_stream_url.py" "${stream_key}" --ttl "${OTTS_AUTH_TTL_SECONDS:-3600}" --host 127.0.0.1 --rtmp-port "${OTTS_RTMP_PORT}" \
      | awk -F': ' '$1 == "rtmp_play" { print $2 }'
  else
    printf 'rtmp://127.0.0.1:%s/%s' "${OTTS_RTMP_PORT}" "${stream_key}"
  fi
}

otts_rtsp_publish_url() {
  local stream_key="$1"
  local mount="${stream_key//\//__}.sdp"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" ]]; then
    printf 'rtsp://127.0.0.1:%s/%s?token=%s' "${OTTS_RTSP_PUBLISH_PORT}" "${mount}" "${OTTS_STREAM_TOKEN}"
  elif [[ -n "${OTTS_AUTH_SECRET:-}" ]]; then
    OTTS_AUTH_SECRET="${OTTS_AUTH_SECRET}" "${SMOKE_DIR}/sign_stream_url.py" "${stream_key}" --ttl "${OTTS_AUTH_TTL_SECONDS:-3600}" --host 127.0.0.1 --rtsp-publish-port "${OTTS_RTSP_PUBLISH_PORT}" \
      | awk -F': ' '$1 == "rtsp_publish" { print $2 }'
  else
    printf 'rtsp://127.0.0.1:%s/%s' "${OTTS_RTSP_PUBLISH_PORT}" "${mount}"
  fi
}

otts_rtsp_play_url() {
  local stream_key="$1"
  local mount="${stream_key//\//__}.sdp"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" ]]; then
    printf 'rtsp://127.0.0.1:%s/%s?token=%s' "${OTTS_RTSP_PLAY_PORT}" "${mount}" "${OTTS_STREAM_TOKEN}"
  elif [[ -n "${OTTS_AUTH_SECRET:-}" ]]; then
    OTTS_AUTH_SECRET="${OTTS_AUTH_SECRET}" "${SMOKE_DIR}/sign_stream_url.py" "${stream_key}" --ttl "${OTTS_AUTH_TTL_SECONDS:-3600}" --host 127.0.0.1 --rtsp-play-port "${OTTS_RTSP_PLAY_PORT}" \
      | awk -F': ' '$1 == "rtsp_play" { print $2 }'
  else
    printf 'rtsp://127.0.0.1:%s/%s' "${OTTS_RTSP_PLAY_PORT}" "${mount}"
  fi
}

otts_srt_publish_url() {
  local port="${1:-${OTTS_SRT_PUBLISH_PORT_BASE}}"
  local url="srt://127.0.0.1:${port}?mode=caller&transtype=live"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" && ${#OTTS_STREAM_TOKEN} -ge 10 && ${#OTTS_STREAM_TOKEN} -le 79 ]]; then
    url="${url}&passphrase=${OTTS_STREAM_TOKEN}&pbkeylen=16"
  fi
  printf '%s' "${url}"
}

otts_srt_play_url() {
  local port="${1:-${OTTS_SRT_PLAY_PORT_BASE}}"
  local url="srt://127.0.0.1:${port}?mode=caller&transtype=live"
  if [[ -n "${OTTS_STREAM_TOKEN:-}" && ${#OTTS_STREAM_TOKEN} -ge 10 && ${#OTTS_STREAM_TOKEN} -le 79 ]]; then
    url="${url}&passphrase=${OTTS_STREAM_TOKEN}&pbkeylen=16"
  fi
  printf '%s' "${url}"
}
