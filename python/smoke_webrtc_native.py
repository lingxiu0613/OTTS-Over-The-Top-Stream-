#!/usr/bin/env python3
import argparse
import asyncio
import fractions
import time


try:
    import aiohttp
    from aiortc import RTCPeerConnection, RTCSessionDescription, RTCRtpSender, VideoStreamTrack
    from av import VideoFrame
except Exception as exc:  # pragma: no cover - smoke environment guard
    print(f"[OTTS] WHIP/WHEP smoke skipped: missing Python WebRTC dependency: {exc}")
    raise SystemExit(0)


class SyntheticVideo(VideoStreamTrack):
    def __init__(self, width=640, height=360, fps=20):
        super().__init__()
        self.width = width
        self.height = height
        self.fps = fps
        self.index = 0
        self.started = time.time()

    async def recv(self):
        await asyncio.sleep(1 / self.fps)
        frame = VideoFrame(width=self.width, height=self.height, format="yuv420p")
        y_value = (self.index * 3) % 255
        u_value = 96
        v_value = 160
        for plane, value in zip(frame.planes, (y_value, u_value, v_value)):
            plane.update(bytes([value]) * plane.buffer_size)
        frame.pts = self.index
        frame.time_base = fractions.Fraction(1, self.fps)
        self.index += 1
        return frame


async def post_sdp(url, sdp):
    async with aiohttp.ClientSession() as session:
        async with session.post(url, data=sdp, headers={"Content-Type": "application/sdp"}) as response:
            body = await response.text()
            location = response.headers.get("Location")
            print(f"[OTTS] POST {url} -> {response.status} location={location or '-'}")
            if response.status not in (200, 201):
                raise RuntimeError(body)
            return body, location


async def delete_session(base_url, location):
    if not location:
        return
    url = location if location.startswith("http") else base_url.rstrip("/") + location
    async with aiohttp.ClientSession() as session:
        async with session.delete(url) as response:
            print(f"[OTTS] DELETE {url} -> {response.status}")


def prefer_h264(transceiver):
    capabilities = RTCRtpSender.getCapabilities("video")
    h264 = [codec for codec in capabilities.codecs if codec.mimeType.lower() == "video/h264"]
    rest = [codec for codec in capabilities.codecs if codec.mimeType.lower() != "video/h264"]
    if h264:
        transceiver.setCodecPreferences(h264 + rest)


async def publish(base_url, stream_key, duration):
    pc = RTCPeerConnection()
    transceiver = pc.addTransceiver(SyntheticVideo(), direction="sendonly")
    prefer_h264(transceiver)
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    url = f"{base_url}/rtc/v1/whip/?app={stream_key.split('/', 1)[0]}&stream={stream_key.split('/', 1)[1]}"
    answer, location = await post_sdp(url, pc.localDescription.sdp)
    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer, type="answer"))
    await asyncio.sleep(duration)
    await delete_session(base_url, location)
    await pc.close()


async def play(base_url, stream_key, timeout):
    pc = RTCPeerConnection()
    counts = {"video": 0, "audio": 0}

    @pc.on("track")
    def on_track(track):
        async def receive():
            end = time.time() + timeout
            while time.time() < end:
                try:
                    await asyncio.wait_for(track.recv(), timeout=2)
                except Exception:
                    break
                counts[track.kind] = counts.get(track.kind, 0) + 1

        asyncio.ensure_future(receive())

    prefer_h264(pc.addTransceiver("video", direction="recvonly"))
    pc.addTransceiver("audio", direction="recvonly")
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    url = f"{base_url}/rtc/v1/whep/?app={stream_key.split('/', 1)[0]}&stream={stream_key.split('/', 1)[1]}"
    answer, location = await post_sdp(url, pc.localDescription.sdp)
    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer, type="answer"))
    await asyncio.sleep(timeout)
    await delete_session(base_url, location)
    await pc.close()
    print(f"[OTTS] WHEP received frames: {counts}")
    if counts["video"] <= 0:
        raise RuntimeError("WHEP did not receive video frames")


async def main():
    parser = argparse.ArgumentParser(description="Native WHIP/WHEP smoke test for OTTS")
    parser.add_argument("--base-url", default="http://127.0.0.1:1985")
    parser.add_argument("--stream-key", default="live/webrtc-smoke")
    parser.add_argument("--duration", type=float, default=8.0)
    args = parser.parse_args()
    if "/" not in args.stream_key:
        raise SystemExit("stream key must look like app/stream")
    publisher = asyncio.create_task(publish(args.base_url, args.stream_key, args.duration + 2))
    await asyncio.sleep(2)
    await play(args.base_url, args.stream_key, args.duration)
    await publisher
    print("[OTTS] native WHIP/WHEP smoke OK")


if __name__ == "__main__":
    asyncio.run(main())
