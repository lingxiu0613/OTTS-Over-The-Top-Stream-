#!/usr/bin/env python3
"""Verify that native WHIP/WHEP negotiate an offered H.265 payload type."""

import argparse
import asyncio

import aiohttp
from aiortc import RTCPeerConnection


def add_h265_to_video_offer(sdp: str, payload_type: int = 120) -> str:
    lines = sdp.replace("\r\n", "\n").splitlines()
    output = []
    inserted = False
    in_video = False
    for line in lines:
        if line.startswith("m=video "):
            fields = line.split()
            fields.insert(3, str(payload_type))
            line = " ".join(fields)
            in_video = True
        elif line.startswith("m="):
            in_video = False
        output.append(line)
        if in_video and line.startswith("a=mid:") and not inserted:
            output.append(f"a=rtpmap:{payload_type} H265/90000")
            inserted = True
    if not inserted:
        raise RuntimeError("video media section not found in SDP offer")
    return "\r\n".join(output) + "\r\n"


async def negotiate(base_url: str, stream_key: str, endpoint: str) -> None:
    pc = RTCPeerConnection()
    direction = "sendonly" if endpoint == "whip" else "recvonly"
    pc.addTransceiver("video", direction=direction)
    pc.addTransceiver("audio", direction=direction)
    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)
    hevc_offer = add_h265_to_video_offer(pc.localDescription.sdp)
    app, stream = stream_key.split("/", 1)
    url = f"{base_url.rstrip('/')}/rtc/v1/{endpoint}/?app={app}&stream={stream}"
    location = None
    async with aiohttp.ClientSession() as session:
        async with session.post(url, data=hevc_offer, headers={"Content-Type": "application/sdp"}) as response:
            answer = await response.text()
            location = response.headers.get("Location")
            if response.status not in (200, 201):
                raise RuntimeError(f"{endpoint.upper()} returned {response.status}: {answer}")
            if "H265/90000" not in answer.upper():
                raise RuntimeError(f"{endpoint.upper()} answer did not negotiate H.265")
            print(f"[OTTS] {endpoint.upper()} H.265 SDP negotiated: status={response.status} location={location}")
        if location:
            delete_url = location if location.startswith("http") else base_url.rstrip("/") + location
            async with session.delete(delete_url) as response:
                print(f"[OTTS] DELETE {delete_url} -> {response.status}")
    await pc.close()


async def main() -> None:
    parser = argparse.ArgumentParser(description="OTTS H.265 WHIP/WHEP SDP smoke test")
    parser.add_argument("--base-url", default="http://127.0.0.1:1985")
    parser.add_argument("--whep-stream-key", default="live/hevc-smoke-source")
    parser.add_argument("--whip-stream-key", default="live/hevc-smoke-publish")
    args = parser.parse_args()
    await negotiate(args.base_url, args.whep_stream_key, "whep")
    await negotiate(args.base_url, args.whip_stream_key, "whip")
    print("[OTTS] H.265 WHIP/WHEP SDP smoke OK")


if __name__ == "__main__":
    asyncio.run(main())
