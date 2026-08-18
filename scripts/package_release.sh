#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-v0.1.0}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_NAME="otts-linux-x64-${VERSION}"
WORK_DIR="/tmp/otts-release"
PACKAGE_DIR="${WORK_DIR}/otts-linux-x64"
ARCHIVE_PATH="${WORK_DIR}/${PACKAGE_NAME}.tar.gz"

cd "${ROOT_DIR}"

if [[ ! -x build/otts_rtmp ]]; then
  echo "[OTTS] missing build/otts_rtmp; build or copy the native executable first" >&2
  exit 1
fi

mkdir -p "${WORK_DIR}"
rm -rf "${PACKAGE_DIR}" "${ARCHIVE_PATH}"
mkdir -p "${PACKAGE_DIR}"

copy_path() {
  local src="$1"
  local dst="${2:-${src}}"
  if [[ -e "${src}" ]]; then
    mkdir -p "${PACKAGE_DIR}/$(dirname "${dst}")"
    cp -a "${src}" "${PACKAGE_DIR}/${dst}"
  fi
}

mkdir -p "${PACKAGE_DIR}/build"
cp -a build/otts_rtmp "${PACKAGE_DIR}/build/otts_rtmp"
copy_path node
copy_path python
copy_path scripts
copy_path docs
copy_path README.md
copy_path restart_otts.sh
copy_path stop_otts.sh
copy_path status_otts.sh
copy_path smoke_rtmp.sh

mkdir -p "${PACKAGE_DIR}/config"
if [[ -f config/otts.env.example ]]; then
  cp -a config/otts.env.example "${PACKAGE_DIR}/config/otts.env"
  cp -a config/otts.env.example "${PACKAGE_DIR}/config/otts.env.example"
fi

cat > "${PACKAGE_DIR}/install_deps_ubuntu.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

sudo apt update
sudo apt install -y \
  ffmpeg curl net-tools iproute2 psmisc nodejs npm \
  libssl3 libsrt1.5-openssl libnice10 libsrtp2-1 libusrsctp2
EOF

cat > "${PACKAGE_DIR}/RUNNING.md" <<'EOF'
# OTTS Runnable Package

## Install runtime dependencies

```bash
./install_deps_ubuntu.sh
```

If your Ubuntu does not provide `libsrt1.5-openssl`, install `libsrt1.4-openssl` instead.

## Configure

Edit `config/otts.env` and set `OTTS_PUBLIC_HOST`, `OTTS_RTSP_PUBLIC_HOST`, and
`OTTS_SRT_PUBLIC_HOST` to your server IP.

## Start

```bash
./restart_otts.sh
```

## Status

```bash
./status_otts.sh
```

## Web console

```text
http://YOUR_SERVER_IP:3000/
https://YOUR_SERVER_IP:3443/
```

## Common test URLs

RTMP publish:

```text
rtmp://YOUR_SERVER_IP:1935/live/nolo001
```

HTTP-FLV play:

```text
http://YOUR_SERVER_IP:8080/live/nolo001.flv
```

RTSP publish:

```text
rtsp://YOUR_SERVER_IP:8554/live__nolo001.sdp
```

RTSP play:

```text
rtsp://YOUR_SERVER_IP:8556/live__nolo001.sdp
```

SRT publish:

```text
srt://YOUR_SERVER_IP:9000?mode=caller&transtype=live
```

SRT play:

```text
srt://YOUR_SERVER_IP:10000?mode=caller&transtype=live
```

WHIP publish:

```text
http://YOUR_SERVER_IP:1985/rtc/v1/whip/?app=live&stream=livestream
```

WHEP play:

```text
http://YOUR_SERVER_IP:1985/rtc/v1/whep/?app=live&stream=livestream
```
EOF

find "${PACKAGE_DIR}" -type f -name "*.sh" -exec chmod +x {} \;
chmod +x "${PACKAGE_DIR}/build/otts_rtmp"

tar -czf "${ARCHIVE_PATH}" -C "${WORK_DIR}" otts-linux-x64

echo "[OTTS] release package created:"
ls -lh "${ARCHIVE_PATH}"
