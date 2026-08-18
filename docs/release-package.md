# OTTS Release Package

This document describes how to publish a runnable Linux x64 package to GitHub Releases.

## Package already built

The release archive is generated on the Ubuntu build machine:

```bash
/tmp/otts-release/otts-linux-x64-v0.2.0.tar.gz
```

The archive contains the compiled C++ core, Node control plane, Python tools, scripts,
default config, one-click smoke scripts, and Node dependencies. It does not contain
`.git`, build directories, local source dependency tarballs, or test media files.

## Create the package

From the project root:

```bash
cd ~/Downloads/otts
bash scripts/package_release.sh v0.2.0
```

The package will be written to:

```bash
/tmp/otts-release/otts-linux-x64-v0.2.0.tar.gz
```

## Run on a new Ubuntu machine

Install system runtime dependencies:

```bash
sudo apt update
sudo apt install -y \
  ffmpeg curl net-tools iproute2 psmisc nodejs npm \
  libssl3 libsrt1.5-openssl libnice10 libsrtp2-1 libusrsctp2
```

If `libsrt1.5-openssl` is not available on your Ubuntu version, try:

```bash
sudo apt install -y libsrt1.4-openssl
```

Download and extract the release archive:

```bash
tar -xzf otts-linux-x64-v0.2.0.tar.gz
cd otts-linux-x64
```

Edit the public host if needed:

```bash
nano config/otts.env
```

Start OTTS:

```bash
./restart_otts.sh
```

Check status:

```bash
./status_otts.sh
```

Open the control panel:

```text
http://YOUR_SERVER_IP:3000/
https://YOUR_SERVER_IP:3443/
```

## Publish to GitHub Releases from the browser

1. Open the GitHub repository page.
2. Click `Releases` -> `Create a new release`.
3. Create a tag, for example `v0.2.0`.
4. Use release title `OTTS v0.2.0 Linux x64`.
5. Upload `/tmp/otts-release/otts-linux-x64-v0.2.0.tar.gz` as the release asset.
6. Click `Publish release`.

Suggested release notes:

```text
OTTS v0.2.0 Linux x64 runnable package.

Includes:
- C++ media core
- Node control panel
- Python smoke/test tools
- RTMP / HTTP-FLV / HLS
- Native RTSP publish/play
- Native SRT publish/play
- Native WHIP/WHEP H.264 + Opus path
- Production config: config/otts.config.json
- HTTP callbacks scaffold
- HLS readiness/startup stabilization
- FLV/MP4 recording lifecycle hardening
- Prometheus-style /metrics
- One-click smoke/regression suite

Run:
tar -xzf otts-linux-x64-v0.2.0.tar.gz
cd otts-linux-x64
./restart_otts.sh
./scripts/smoke_all.sh
```

## Publish with GitHub CLI

If `gh` is installed and authenticated on the Ubuntu machine:

```bash
cd ~/Downloads/otts
git tag v0.2.0
git push origin v0.2.0
gh release create v0.2.0 \
  /tmp/otts-release/otts-linux-x64-v0.2.0.tar.gz \
  --title "OTTS v0.2.0 Linux x64" \
  --notes-file docs/release-package.md
```

If the tag already exists, skip the `git tag` command.
