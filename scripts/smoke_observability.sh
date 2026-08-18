#!/usr/bin/env bash
set -euo pipefail

API_BASE="${OTTS_API_BASE:-http://127.0.0.1:8080}"

echo "[OTTS] observability/config smoke test"

echo "[OTTS] system config:"
curl -fsS "${API_BASE}/api/system/status" >/tmp/otts_system_status.json
jq -e '.ok == true and .config.loaded == true and .ports.rtmp > 0 and .ports.http_api > 0' /tmp/otts_system_status.json >/dev/null
jq '{config, ports, maintenance}' /tmp/otts_system_status.json

echo "[OTTS] metrics:"
curl -fsS "${API_BASE}/metrics" >/tmp/otts_metrics.prom
grep -q '^otts_streams_total ' /tmp/otts_metrics.prom
grep -q '^otts_publishers_online ' /tmp/otts_metrics.prom
grep -q '^otts_cleanup_runs_total ' /tmp/otts_metrics.prom
head -n 40 /tmp/otts_metrics.prom
