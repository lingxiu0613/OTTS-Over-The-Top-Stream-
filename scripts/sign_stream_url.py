#!/usr/bin/env python3
import argparse
import hashlib
import hmac
import os
import time
from pathlib import Path
from urllib.parse import quote, urlencode


def load_env_file(path: Path) -> None:
    if not path.exists():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def sign(secret: str, action: str, stream_key: str, expires: str) -> str:
    payload = f"{action}\n{stream_key}\n{expires}".encode("utf-8")
    return hmac.new(secret.encode("utf-8"), payload, hashlib.sha256).hexdigest()


def append_auth(url: str, secret: str, action: str, stream_key: str, ttl: int) -> str:
    expires = str(int(time.time()) + max(1, ttl))
    params = urlencode({"expires": expires, "sign": sign(secret, action, stream_key, expires)})
    return f"{url}{'&' if '?' in url else '?'}{params}"


def main() -> None:
    load_env_file(Path(__file__).resolve().parents[1] / "config" / "otts.env")
    parser = argparse.ArgumentParser(description="Generate OTTS stream signed URLs")
    parser.add_argument("stream_key", help="stream key, for example live/nolo001")
    parser.add_argument("--secret", default=os.environ.get("OTTS_AUTH_SECRET", ""), help="auth secret, defaults to OTTS_AUTH_SECRET")
    parser.add_argument("--ttl", type=int, default=3600, help="signature TTL seconds")
    parser.add_argument("--host", default="192.168.40.11", help="public host")
    parser.add_argument("--rtmp-port", type=int, default=1935)
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--webrtc-port", type=int, default=1985)
    parser.add_argument("--rtsp-publish-port", type=int, default=8554)
    parser.add_argument("--rtsp-play-port", type=int, default=8556)
    args = parser.parse_args()
    if not args.secret:
        raise SystemExit("missing --secret or OTTS_AUTH_SECRET")

    stream_key = args.stream_key.strip("/")
    mount = stream_key.replace("/", "__")
    encoded_stream_key = quote(stream_key, safe="")
    app, _, stream = stream_key.partition("/")
    stream = stream or app
    app = app if "/" in stream_key else "live"

    urls = {
        "rtmp_publish": append_auth(f"rtmp://{args.host}:{args.rtmp_port}/{stream_key}", args.secret, "publish", stream_key, args.ttl),
        "rtmp_play": append_auth(f"rtmp://{args.host}:{args.rtmp_port}/{stream_key}", args.secret, "play", stream_key, args.ttl),
        "http_flv_play": append_auth(f"http://{args.host}:{args.http_port}/{stream_key}.flv", args.secret, "play", stream_key, args.ttl),
        "whip_publish": append_auth(f"http://{args.host}:{args.webrtc_port}/rtc/v1/whip/?app={quote(app)}&stream={quote(stream)}", args.secret, "publish", stream_key, args.ttl),
        "whep_play": append_auth(f"http://{args.host}:{args.webrtc_port}/rtc/v1/whep/?app={quote(app)}&stream={quote(stream)}", args.secret, "play", stream_key, args.ttl),
        "rtsp_publish": append_auth(f"rtsp://{args.host}:{args.rtsp_publish_port}/{mount}.sdp", args.secret, "publish", stream_key, args.ttl),
        "rtsp_play": append_auth(f"rtsp://{args.host}:{args.rtsp_play_port}/{mount}.sdp", args.secret, "play", stream_key, args.ttl),
    }
    for name, url in urls.items():
        print(f"{name}: {url}")


if __name__ == "__main__":
    main()
