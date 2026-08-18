# OTTS Production Config and HTTP Callback

This document covers the production configuration entry point and business callback hooks.

## Config Files

OTTS now uses two layers:

- `config/otts.env`: bootstrap environment for scripts and process startup.
- `config/otts.config.json`: runtime production config for ports, protocols, auth, recording, HLS, logging, callbacks, and vhost/app/stream policy.

Create the runtime config:

```bash
cd ~/Downloads/otts
cp config/otts.config.json.example config/otts.config.json
```

The default path is exported by `scripts/otts_env.sh` as:

```bash
OTTS_CONFIG_JSON=~/Downloads/otts/config/otts.config.json
```

## Hot Reload

Node watches `config/otts.config.json` and reloads it automatically. You can also reload manually:

```bash
curl -X POST http://127.0.0.1:3000/api/config/reload
```

Runtime hot-reload applies to:

- HLS root directory, auto start, segment duration, playlist size, cleanup and idle timeout.
- Recording root directory, default format, enabled flag, auto record.
- Callback enable flag, URLs, timeout and retry policy.
- Vhost/app/stream policy resolution exposed through API.

Restart-required settings:

- TCP/UDP ports.
- Public host values used by already-running protocol services.
- Native protocol process mode changes.
- C++ core auth values that are read at process startup.

Restart after changing those:

```bash
./restart_otts.sh
```

## Inspect Config

```bash
curl http://127.0.0.1:3000/api/config
curl "http://127.0.0.1:3000/api/config/stream?stream_key=live/nolo001"
```

Stream policy is resolved in this order:

1. Global defaults.
2. Vhost defaults.
3. App defaults.
4. Stream defaults.

## HTTP Callback Events

Enable callbacks in `config/otts.config.json`:

```json
{
  "callbacks": {
    "enabled": true,
    "timeoutMs": 3000,
    "retries": 1,
    "events": {
      "on_publish": ["http://127.0.0.1:9009/otts/on_publish"],
      "on_unpublish": ["http://127.0.0.1:9009/otts/on_unpublish"],
      "on_play": ["http://127.0.0.1:9009/otts/on_play"],
      "on_stop": ["http://127.0.0.1:9009/otts/on_stop"],
      "on_dvr": ["http://127.0.0.1:9009/otts/on_dvr"],
      "on_hls": ["http://127.0.0.1:9009/otts/on_hls"]
    }
  }
}
```

Each callback is sent as HTTP `POST` with JSON:

```json
{
  "event": "on_publish",
  "timestamp": "2026-08-18T10:00:00.000Z",
  "stream_key": "live/nolo001",
  "vhost": "__default__",
  "app": "live",
  "stream": "nolo001",
  "source_protocol": "rtmp",
  "client_ip": null,
  "session_id": null,
  "data": {}
}
```

Event meanings:

- `on_publish`: stream publisher appears.
- `on_unpublish`: stream publisher disappears.
- `on_play`: viewer count changes from zero to non-zero.
- `on_stop`: viewer count returns to zero.
- `on_dvr`: recording starts, stops, or auto-record action reports an error.
- `on_hls`: HLS starts, stops, becomes ready, or cleanup runs.

Inspect callback delivery history:

```bash
curl http://127.0.0.1:3000/api/callbacks/events
curl http://127.0.0.1:3000/api/callbacks/config
```
