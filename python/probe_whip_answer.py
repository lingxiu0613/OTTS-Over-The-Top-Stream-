#!/usr/bin/env python3

import argparse
import asyncio

from aiohttp import ClientSession
from aiortc import RTCPeerConnection


def summarize_sdp(sdp: str) -> str:
    lines = []
    for raw in sdp.splitlines():
        line = raw.strip()
        if line.startswith("m=") or line in {"a=sendonly", "a=recvonly", "a=sendrecv", "a=inactive"}:
            lines.append(line)
    return "\n".join(lines)


async def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    args = parser.parse_args()

    pc = RTCPeerConnection()
    pc.addTransceiver("audio", direction="sendonly")
    pc.addTransceiver("video", direction="sendonly")

    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)

    async with ClientSession() as session:
        async with session.post(
            args.url,
            data=pc.localDescription.sdp,
            headers={"Content-Type": "application/sdp"},
        ) as resp:
            body = await resp.text()
            print("STATUS", resp.status)
            print("LOCATION", resp.headers.get("Location", ""))
            print("ANSWER_SUMMARY_BEGIN")
            print(summarize_sdp(body))
            print("ANSWER_SUMMARY_END")
            if resp.status == 201:
                await pc.setRemoteDescription(type("Desc", (), {"type": "answer", "sdp": body})())

    await pc.close()


if __name__ == "__main__":
    asyncio.run(main())
