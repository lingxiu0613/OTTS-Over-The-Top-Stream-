#!/usr/bin/env python3

import argparse
import asyncio
from fractions import Fraction
import logging
import time
import uuid
from dataclasses import dataclass, field
from typing import Any
from urllib.parse import urlencode

from aiohttp import ClientSession, web
from aiortc import MediaStreamTrack, RTCPeerConnection, RTCSessionDescription
from aiortc.contrib.media import MediaRecorder, MediaRelay
from aiortc.sdp import candidate_from_sdp


logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s %(message)s")


def now_ms() -> int:
    return int(time.time() * 1000)


def summarize_sdp(sdp: str) -> str:
    summary: list[str] = []
    for raw_line in sdp.splitlines():
        line = raw_line.strip()
        if (
            line.startswith("m=")
            or line.startswith("a=group:BUNDLE")
            or line.startswith("a=mid:")
            or line.startswith("a=setup:")
            or line.startswith("a=ice-ufrag:")
            or line.startswith("a=fingerprint:")
            or line in {"a=sendonly", "a=recvonly", "a=sendrecv", "a=inactive"}
        ):
            summary.append(line)
    return " | ".join(summary[:16])


def force_server_dtls_role(pc: RTCPeerConnection) -> None:
    # OBS WHIP is stricter than browsers about the answer-side DTLS role.
    # aiortc defaults answer generation to "client" (setup:active) unless
    # the transport role is preset, so pin it to server (setup:passive).
    for transceiver in pc.getTransceivers():
        transport = getattr(transceiver, "_transport", None)
        if transport is not None and hasattr(transport, "_set_role"):
            transport._set_role("server")


async def wait_for_ice_complete(pc: RTCPeerConnection, timeout: float = 5.0) -> None:
    deadline = time.time() + timeout
    while pc.iceGatheringState != "complete" and time.time() < deadline:
        await asyncio.sleep(0.1)


class NormalizedTrack(MediaStreamTrack):
    def __init__(self, source: MediaStreamTrack) -> None:
        super().__init__()
        self.kind = source.kind
        self._source = source
        self._next_pts = 0
        self._last_source_time: float | None = None
        self._video_time_base = Fraction(1, 90000)
        self._audio_time_base = Fraction(1, 48000)

    async def recv(self):
        frame = await self._source.recv()
        source_time = getattr(frame, "time", None)

        if self.kind == "video":
            delta_seconds = self._compute_video_delta(source_time)
            pts_increment = max(1, int(delta_seconds / float(self._video_time_base)))
            frame.pts = self._next_pts
            frame.time_base = self._video_time_base
            self._next_pts += pts_increment
        else:
            sample_rate = int(getattr(frame, "sample_rate", 48000) or 48000)
            time_base = Fraction(1, sample_rate)
            samples = int(getattr(frame, "samples", 0) or 0)
            if samples <= 0:
                delta_seconds = self._compute_audio_delta(source_time)
                samples = max(1, int(delta_seconds * sample_rate))
            frame.pts = self._next_pts
            frame.time_base = time_base
            self._next_pts += samples

        if source_time is not None:
            self._last_source_time = float(source_time)
        return frame

    def _compute_video_delta(self, source_time: float | None) -> float:
        if self._last_source_time is None or source_time is None:
            return 1.0 / 30.0
        delta = float(source_time) - self._last_source_time
        if delta <= 0 or delta > 0.2:
            return 1.0 / 30.0
        return delta

    def _compute_audio_delta(self, source_time: float | None) -> float:
        if self._last_source_time is None or source_time is None:
            return 0.02
        delta = float(source_time) - self._last_source_time
        if delta <= 0 or delta > 0.2:
            return 0.02
        return delta


def parse_offer_codecs(offer_sdp: str) -> dict[str, str]:
    codecs: dict[str, str] = {}
    payload_map: dict[str, dict[str, str]] = {"audio": {}, "video": {}}
    first_payloads: dict[str, str] = {}
    current_kind = ""

    for raw_line in offer_sdp.splitlines():
        line = raw_line.strip()
        if line.startswith("m=audio "):
            current_kind = "audio"
            parts = line.split()
            if len(parts) > 3:
                first_payloads[current_kind] = parts[3]
        elif line.startswith("m=video "):
            current_kind = "video"
            parts = line.split()
            if len(parts) > 3:
                first_payloads[current_kind] = parts[3]
        elif line.startswith("m="):
            current_kind = ""
        elif current_kind and line.startswith("a=rtpmap:"):
            mapping = line[len("a=rtpmap:") :]
            if " " not in mapping:
                continue
            payload_type, codec_desc = mapping.split(" ", 1)
            payload_map[current_kind][payload_type.strip()] = codec_desc.split("/", 1)[0].strip().lower()

    for kind in ("audio", "video"):
        codecs[kind] = payload_map[kind].get(first_payloads.get(kind, ""), "")
    return codecs


@dataclass
class PublishedStream:
    stream_key: str
    session_id: str
    pc: RTCPeerConnection
    tracks: dict[str, MediaStreamTrack] = field(default_factory=dict)
    bridge_url: str = ""
    recorder: MediaRecorder | None = None
    recorder_started: bool = False
    audio_codec: str = ""
    video_codec: str = ""
    track_ready: asyncio.Event = field(default_factory=asyncio.Event)
    last_error: str = ""
    created_at_ms: int = field(default_factory=now_ms)
    updated_at_ms: int = field(default_factory=now_ms)


@dataclass
class SessionInfo:
    session_id: str
    stream_key: str
    direction: str
    pc: RTCPeerConnection
    state: str = "new"
    created_at_ms: int = field(default_factory=now_ms)
    updated_at_ms: int = field(default_factory=now_ms)
    last_error: str = ""
    bridge_url: str = ""
    offer_size: int = 0
    answer_size: int = 0

    def snapshot(self) -> dict[str, Any]:
        return {
            "session_id": self.session_id,
            "stream_key": self.stream_key,
            "direction": self.direction,
            "state": self.state,
            "created_at_epoch_ms": self.created_at_ms,
            "updated_at_epoch_ms": self.updated_at_ms,
            "last_error": self.last_error,
            "bridge_url": self.bridge_url,
            "offer_size": self.offer_size,
            "answer_size": self.answer_size,
        }


class GatewayState:
    def __init__(self, core_rtmp_base: str, enable_rtmp_bridge: bool) -> None:
        self.relay = MediaRelay()
        self.publishers: dict[str, PublishedStream] = {}
        self.sessions: dict[str, SessionInfo] = {}
        self.core_rtmp_base = core_rtmp_base.rstrip("/")
        self.enable_rtmp_bridge = enable_rtmp_bridge

    def prune_closed_sessions(self) -> None:
        removable = {
            session_id
            for session_id, info in self.sessions.items()
            if info.state in {"closed", "failed"} and not any(
                publisher.session_id == session_id for publisher in self.publishers.values()
            )
        }
        for session_id in removable:
            self.sessions.pop(session_id, None)

    async def sync_core_stream(self, publisher: PublishedStream, has_publisher: bool) -> None:
        if has_publisher:
            params = urlencode(
                {
                    "stream_key": publisher.stream_key,
                    "source_protocol": "whip",
                    "audio_codec": publisher.audio_codec,
                    "video_codec": publisher.video_codec,
                    "managed_by": "python-webrtc-gateway",
                    "has_publisher": "true",
                }
            )
            url = f"http://127.0.0.1:8080/api/internal/streams/upsert?{params}"
        else:
            params = urlencode(
                {
                    "stream_key": publisher.stream_key,
                    "source_protocol": "whip",
                }
            )
            url = f"http://127.0.0.1:8080/api/internal/streams/remove?{params}"

        try:
            async with ClientSession() as session:
                async with session.post(url) as response:
                    await response.text()
        except Exception as exc:
            publisher.last_error = f"core sync failed: {exc}"
            publisher.updated_at_ms = now_ms()

    async def sync_core_viewers(self, stream_key: str) -> None:
        viewer_count = sum(
            1
            for info in self.sessions.values()
            if info.stream_key == stream_key and info.direction == "whep" and info.state in {"connecting", "connected"}
        )
        params = urlencode(
            {
                "stream_key": stream_key,
                "source_protocol": "whip",
                "managed_by": "python-webrtc-gateway",
                "viewer_count": str(viewer_count),
            }
        )
        url = f"http://127.0.0.1:8080/api/internal/streams/viewers?{params}"
        try:
            async with ClientSession() as session:
                async with session.post(url) as response:
                    await response.text()
        except Exception:
            pass

    async def start_bridge(self, stream_key: str) -> None:
        if not self.enable_rtmp_bridge:
            return
        publisher = self.publishers.get(stream_key)
        if not publisher or publisher.recorder is None or publisher.recorder_started or not publisher.tracks:
            return
        try:
            await publisher.recorder.start()
            publisher.recorder_started = True
        except Exception as exc:
            publisher.last_error = str(exc)
            publisher.updated_at_ms = now_ms()

    async def wait_for_publisher_tracks(self, stream_key: str, timeout: float = 5.0) -> PublishedStream | None:
        publisher = self.publishers.get(stream_key)
        if not publisher:
            return None
        if publisher.tracks:
            return publisher
        try:
            await asyncio.wait_for(publisher.track_ready.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            return None
        return self.publishers.get(stream_key)

    async def close_session(self, session_id: str) -> bool:
        info = self.sessions.get(session_id)
        if not info:
            return False
        await info.pc.close()
        info.state = "closed"
        info.updated_at_ms = now_ms()
        for stream_key, publisher in list(self.publishers.items()):
            if publisher.session_id == session_id:
                if publisher.recorder_started and publisher.recorder is not None:
                    await publisher.recorder.stop()
                await self.sync_core_stream(publisher, False)
                self.publishers.pop(stream_key, None)
            await self.sync_core_viewers(stream_key)
        self.prune_closed_sessions()
        return True

    def stream_summaries(self) -> list[dict[str, Any]]:
        streams: dict[str, dict[str, Any]] = {}

        for session in self.sessions.values():
            entry = streams.setdefault(
                session.stream_key,
                {
                    "stream_key": session.stream_key,
                    "whip_session_count": 0,
                    "whep_session_count": 0,
                    "session_states": {},
                    "publisher_session_id": None,
                    "bridge_url": "",
                    "tracks": [],
                    "audio_codec": "",
                    "video_codec": "",
                    "bridge_active": False,
                    "recorder_started": False,
                    "last_error": "",
                    "updated_at_epoch_ms": 0,
                },
            )
            if session.direction == "whip":
                entry["whip_session_count"] += 1
                entry["publisher_session_id"] = session.session_id
            elif session.direction == "whep":
                entry["whep_session_count"] += 1
            entry["session_states"][session.direction] = session.state
            entry["bridge_url"] = session.bridge_url or entry["bridge_url"]
            entry["updated_at_epoch_ms"] = max(entry["updated_at_epoch_ms"], session.updated_at_ms)
            if session.last_error:
                entry["last_error"] = session.last_error

        for publisher in self.publishers.values():
            entry = streams.setdefault(
                publisher.stream_key,
                {
                    "stream_key": publisher.stream_key,
                    "whip_session_count": 0,
                    "whep_session_count": 0,
                    "session_states": {},
                    "publisher_session_id": publisher.session_id,
                    "bridge_url": publisher.bridge_url,
                    "tracks": [],
                    "audio_codec": "",
                    "video_codec": "",
                    "bridge_active": False,
                    "recorder_started": False,
                    "last_error": "",
                    "updated_at_epoch_ms": publisher.updated_at_ms,
                },
            )
            entry["publisher_session_id"] = publisher.session_id
            entry["bridge_url"] = publisher.bridge_url
            entry["tracks"] = sorted(publisher.tracks.keys())
            entry["audio_codec"] = publisher.audio_codec
            entry["video_codec"] = publisher.video_codec
            entry["bridge_active"] = publisher.recorder is not None
            entry["recorder_started"] = publisher.recorder_started
            entry["updated_at_epoch_ms"] = max(entry["updated_at_epoch_ms"], publisher.updated_at_ms)
            if publisher.last_error:
                entry["last_error"] = publisher.last_error

        return [streams[key] for key in sorted(streams.keys())]

    def snapshot(self) -> dict[str, Any]:
        self.prune_closed_sessions()
        return {
            "sessions": [session.snapshot() for session in self.sessions.values()],
            "published_streams": [
                {
                    "stream_key": stream.stream_key,
                    "session_id": stream.session_id,
                    "track_kinds": list(stream.tracks.keys()),
                    "bridge_url": stream.bridge_url,
                    "last_error": stream.last_error,
                    "created_at_epoch_ms": stream.created_at_ms,
                    "updated_at_epoch_ms": stream.updated_at_ms,
                }
                for stream in self.publishers.values()
            ],
            "streams": self.stream_summaries(),
        }


STATE: GatewayState


def derive_stream_key(request: web.Request) -> str:
    direct = request.query.get("stream_key")
    if direct:
        return direct.strip("/")

    app = request.query.get("app", "").strip("/")
    stream = request.query.get("stream", "").strip("/")
    name = request.query.get("name", "").strip("/")
    if app and stream:
        return f"{app}/{stream}"
    if app and name:
        return f"{app}/{name}"
    if stream:
        return stream
    if name:
        return name
    return "live/webrtc-demo"


def cors(response: web.StreamResponse) -> web.StreamResponse:
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    response.headers["Access-Control-Allow-Methods"] = "GET,POST,DELETE,OPTIONS"
    return response


async def options_handler(_request: web.Request) -> web.Response:
    return cors(web.Response(status=204))


async def health(_request: web.Request) -> web.Response:
    return cors(web.json_response({"ok": True, "service": "otts-webrtc-gateway"}))


async def status(_request: web.Request) -> web.Response:
    return cors(web.json_response(STATE.snapshot()))


async def delete_session(request: web.Request) -> web.Response:
    session_id = request.match_info["session_id"]
    ok = await STATE.close_session(session_id)
    return cors(web.json_response({"ok": ok, "session_id": session_id}))


async def patch_session(request: web.Request) -> web.Response:
    session_id = request.match_info["session_id"]
    info = STATE.sessions.get(session_id)
    if not info:
        logging.warning("PATCH session not found session_id=%s", session_id)
        return cors(web.Response(status=404, text="session not found"))

    body = await request.text()
    logging.info("PATCH session_id=%s body_len=%d", session_id, len(body))
    current_mid = None
    current_mline = 0

    try:
        for raw_line in body.splitlines():
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("m="):
                current_mline += 1
            elif line.startswith("a=mid:"):
                current_mid = line[6:]
            elif line == "a=end-of-candidates":
                await info.pc.addIceCandidate(None)
            elif line.startswith("a=candidate:"):
                candidate = candidate_from_sdp(line[2:])
                candidate.sdpMid = current_mid
                candidate.sdpMLineIndex = max(0, current_mline - 1)
                await info.pc.addIceCandidate(candidate)
    except Exception as exc:
        info.last_error = str(exc)
        info.updated_at_ms = now_ms()
        return cors(web.Response(status=400, text=f"invalid ICE fragment: {exc}"))

    info.updated_at_ms = now_ms()
    return cors(web.Response(status=204))


async def whip(request: web.Request) -> web.Response:
    stream_key = derive_stream_key(request)
    offer_sdp = await request.text()
    session_id = uuid.uuid4().hex
    logging.info("WHIP start session_id=%s stream_key=%s body_len=%d", session_id, stream_key, len(offer_sdp))

    pc = RTCPeerConnection()
    session = SessionInfo(session_id=session_id, stream_key=stream_key, direction="whip", pc=pc, state="connecting")
    session.offer_size = len(offer_sdp)
    STATE.sessions[session_id] = session

    old = STATE.publishers.get(stream_key)
    if old:
        await old.pc.close()
        old_session = STATE.sessions.get(old.session_id)
        if old_session:
            old_session.state = "replaced"
            old_session.updated_at_ms = now_ms()

    bridge_url = f"{STATE.core_rtmp_base}/{stream_key}"
    offer_codecs = parse_offer_codecs(offer_sdp)
    publisher = PublishedStream(
        stream_key=stream_key,
        session_id=session_id,
        pc=pc,
        bridge_url=bridge_url,
        recorder=MediaRecorder(bridge_url, format="flv") if STATE.enable_rtmp_bridge else None,
        audio_codec=offer_codecs.get("audio", ""),
        video_codec=offer_codecs.get("video", ""),
    )
    session.bridge_url = bridge_url
    STATE.publishers[stream_key] = publisher

    @pc.on("track")
    def on_track(track: MediaStreamTrack) -> None:
        publisher.tracks[track.kind] = track
        publisher.updated_at_ms = now_ms()
        session.updated_at_ms = now_ms()
        if publisher.recorder is not None:
            publisher.recorder.addTrack(STATE.relay.subscribe(track))
        publisher.track_ready.set()
        asyncio.create_task(STATE.start_bridge(stream_key))

        @track.on("ended")
        async def on_ended() -> None:
            publisher.tracks.pop(track.kind, None)
            publisher.updated_at_ms = now_ms()

    @pc.on("connectionstatechange")
    async def on_connectionstatechange() -> None:
        session.state = pc.connectionState
        session.updated_at_ms = now_ms()
        publisher.updated_at_ms = now_ms()
        logging.info("WHIP connection state session_id=%s state=%s", session_id, pc.connectionState)
        if pc.connectionState == "connected":
            await STATE.sync_core_stream(publisher, True)
        if pc.connectionState in {"failed", "closed"}:
            if publisher.recorder_started and publisher.recorder is not None:
                await publisher.recorder.stop()
            await STATE.sync_core_stream(publisher, False)
            STATE.publishers.pop(stream_key, None)
            await STATE.sync_core_viewers(stream_key)
            STATE.prune_closed_sessions()

    @pc.on("iceconnectionstatechange")
    async def on_iceconnectionstatechange() -> None:
        session.updated_at_ms = now_ms()
        logging.info("WHIP ICE state session_id=%s state=%s", session_id, pc.iceConnectionState)
        if pc.iceConnectionState == "failed":
            session.last_error = "ice connection failed"

    try:
        await pc.setRemoteDescription(RTCSessionDescription(sdp=offer_sdp, type="offer"))
        force_server_dtls_role(pc)
        logging.info("WHIP offer session_id=%s %s", session_id, summarize_sdp(offer_sdp))
        answer = await pc.createAnswer()
        await pc.setLocalDescription(answer)
        await wait_for_ice_complete(pc)
        logging.info("WHIP answer session_id=%s %s", session_id, summarize_sdp(pc.localDescription.sdp))
        session.state = "connected"
        session.answer_size = len(pc.localDescription.sdp)
        session.updated_at_ms = now_ms()
    except Exception as exc:
        logging.exception("WHIP failed session_id=%s stream_key=%s", session_id, stream_key)
        session.state = "failed"
        session.last_error = str(exc)
        session.updated_at_ms = now_ms()
        STATE.publishers.pop(stream_key, None)
        await STATE.sync_core_stream(publisher, False)
        await pc.close()
        return cors(web.Response(status=400, text=f"invalid WHIP offer: {exc}"))

    response = web.Response(status=201, text=pc.localDescription.sdp, content_type="application/sdp")
    response.headers["Location"] = f"/session/{session_id}"
    response.headers["X-Session-Id"] = session_id
    logging.info("WHIP ready session_id=%s location=/session/%s", session_id, session_id)
    return cors(response)


async def whep(request: web.Request) -> web.Response:
    stream_key = derive_stream_key(request)
    offer_sdp = await request.text()
    bridge_url = f"{STATE.core_rtmp_base}/{stream_key}"
    logging.info("WHEP start stream_key=%s body_len=%d", stream_key, len(offer_sdp))

    source = await STATE.wait_for_publisher_tracks(stream_key, timeout=5.0)
    if source and source.tracks:
        session_id = uuid.uuid4().hex
        pc = RTCPeerConnection()
        session = SessionInfo(session_id=session_id, stream_key=stream_key, direction="whep", pc=pc, state="connecting")
        session.bridge_url = bridge_url
        session.offer_size = len(offer_sdp)
        STATE.sessions[session_id] = session

        @pc.on("connectionstatechange")
        async def on_connectionstatechange() -> None:
            session.state = pc.connectionState
            session.updated_at_ms = now_ms()
            await STATE.sync_core_viewers(stream_key)
            if pc.connectionState in {"failed", "closed"}:
                STATE.prune_closed_sessions()

        @pc.on("iceconnectionstatechange")
        async def on_iceconnectionstatechange() -> None:
            session.updated_at_ms = now_ms()
            if pc.iceConnectionState == "failed":
                session.last_error = "ice connection failed"

        try:
            await pc.setRemoteDescription(RTCSessionDescription(sdp=offer_sdp, type="offer"))
            for track in source.tracks.values():
                pc.addTrack(NormalizedTrack(STATE.relay.subscribe(track)))

            answer = await pc.createAnswer()
            await pc.setLocalDescription(answer)
            await wait_for_ice_complete(pc)
            session.state = "connected"
            session.answer_size = len(pc.localDescription.sdp)
            session.updated_at_ms = now_ms()
            await STATE.sync_core_viewers(stream_key)
        except Exception as exc:
            session.state = "failed"
            session.last_error = str(exc)
            session.updated_at_ms = now_ms()
            await pc.close()
            await STATE.sync_core_viewers(stream_key)
            STATE.prune_closed_sessions()
            return cors(web.Response(status=400, text=f"invalid WHEP offer: {exc}"))

        response = web.Response(status=201, text=pc.localDescription.sdp, content_type="application/sdp")
        response.headers["Location"] = f"/session/{session_id}"
        response.headers["X-Session-Id"] = session_id
        return cors(response)

    return cors(web.Response(status=404, text="stream not available yet"))


def build_app() -> web.Application:
    app = web.Application()
    app.router.add_route("OPTIONS", "/{tail:.*}", options_handler)
    app.router.add_get("/health", health)
    app.router.add_get("/api/streams", status)
    app.router.add_post("/whip", whip)
    app.router.add_post("/whip/v1", whip)
    app.router.add_post("/rtc/v1/whip", whip)
    app.router.add_post("/rtc/v1/whip/", whip)
    app.router.add_post("/whep", whep)
    app.router.add_post("/whep/v1", whep)
    app.router.add_post("/rtc/v1/whep", whep)
    app.router.add_post("/rtc/v1/whep/", whep)
    app.router.add_patch("/session/{session_id}", patch_session)
    app.router.add_delete("/session/{session_id}", delete_session)
    return app


def main() -> None:
    parser = argparse.ArgumentParser(description="OTTS WHIP/WHEP gateway")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8081)
    parser.add_argument("--core-rtmp-base", default="rtmp://127.0.0.1:1935")
    parser.add_argument("--enable-rtmp-bridge", action="store_true")
    args = parser.parse_args()

    global STATE
    STATE = GatewayState(
        core_rtmp_base=args.core_rtmp_base,
        enable_rtmp_bridge=args.enable_rtmp_bridge,
    )
    web.run_app(build_app(), host=args.host, port=args.port)


if __name__ == "__main__":
    main()
