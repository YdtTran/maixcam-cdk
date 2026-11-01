#!/usr/bin/env python3
"""
Python WebRTC client using aiortc to stream Opus audio to MaixCAM and test Talk downlink playback.
"""
import asyncio
import fractions
import json
import math
import struct
import time
import aiohttp
from aiortc import RTCPeerConnection, RTCSessionDescription, AudioStreamTrack
from av import AudioFrame

class SineAudioTrack(AudioStreamTrack):
    """
    A 48000 Hz stereo audio track that produces a strictly real-time paced sine tone.
    """
    def __init__(self, frequency=440.0, rate=48000):
        super().__init__()
        self.frequency = frequency
        self.rate = rate
        self.time_idx = 0
        self._timestamp = 0
        self._next_send = None
        self.active = False

    async def recv(self):
        while not self.active:
            await asyncio.sleep(0.02)

        now = time.monotonic()
        if self._next_send is None:
            self._next_send = now
        else:
            self._next_send += 0.020
            delay = self._next_send - now
            if delay > 0:
                await asyncio.sleep(delay)
            else:
                self._next_send = now

        # 20ms of audio = 960 samples per channel
        samples_count = 960
        pts = self._timestamp
        time_base = fractions.Fraction(1, self.rate)
        self._timestamp += samples_count

        # Generate stereo interleaved S16 PCM
        raw_samples = []
        for i in range(samples_count):
            t = (self.time_idx + i) / self.rate
            val = int(12000.0 * math.sin(2.0 * math.pi * self.frequency * t))
            raw_samples.extend([val, val])
        self.time_idx += samples_count
        pcm_bytes = struct.pack(f"<{len(raw_samples)}h", *raw_samples)

        frame = AudioFrame(format="s16", layout="stereo", samples=samples_count)
        frame.planes[0].update(pcm_bytes)
        frame.pts = pts
        frame.time_base = time_base
        frame.sample_rate = self.rate
        return frame

async def run_test(target_ip="192.168.1.8", duration=4.0):
    ws_url = f"http://{target_ip}:8001"
    print(f"Connecting to signaling WebSocket at {ws_url}...")

    pc = RTCPeerConnection()
    track = SineAudioTrack()

    session = aiohttp.ClientSession()
    ws = await session.ws_connect(f"ws://{target_ip}:8001")

    # Send request
    await ws.send_str(json.dumps({"id": "server", "type": "request"}))

    # Wait for offer
    msg = await ws.receive()
    data = json.loads(msg.data)
    print("Received offer from server!")

    sdp_text = data["sdp"]
    # Normalize profile-level-id for aiortc compatibility
    sdp_text = sdp_text.replace("4D0034", "42e01f").replace("4d0034", "42e01f")
    # Filter out unreachable internal 10.172.x.x candidates and c= lines
    filtered = []
    for line in sdp_text.splitlines():
        if "10.172." in line:
            if line.startswith("c="):
                filtered.append(f"c=IN IP4 {target_ip}")
            continue
        filtered.append(line)
    sdp_text = "\r\n".join(filtered) + "\r\n"

    offer = RTCSessionDescription(sdp=sdp_text, type=data["type"])
    await pc.setRemoteDescription(offer)

    # Attach our audio track to the offer's audio transceiver
    for transceiver in pc.getTransceivers():
        if transceiver.kind == "audio":
            transceiver.direction = "sendrecv"
            transceiver.sender.replaceTrack(track)
            print("Attached audio track to audio transceiver (direction=sendrecv)")
        elif transceiver.kind == "video":
            transceiver.direction = "recvonly"

    dc = pc.createDataChannel("talk")
    dc_opened = asyncio.Event()

    @dc.on("open")
    def on_dc_open():
        print("DataChannel opened!")
        dc_opened.set()

    @pc.on("connectionstatechange")
    def on_state():
        print(f"PeerConnection state: {pc.connectionState}")

    @pc.on("iceconnectionstatechange")
    def on_ice():
        print(f"ICE connection state: {pc.iceConnectionState}")

    @pc.on("icegatheringstatechange")
    def on_gathering():
        print(f"ICE gathering state: {pc.iceGatheringState}")

    # Create and set answer
    answer = await pc.createAnswer()
    await pc.setLocalDescription(answer)

    # Wait for ICE gathering to complete so candidates are embedded in SDP
    print("Waiting for ICE gathering to complete...")
    while pc.iceGatheringState != "complete":
        await asyncio.sleep(0.05)
    print("ICE gathering complete, candidate lines included.")

    # Send answer with complete candidates
    await ws.send_str(json.dumps({
        "id": "server",
        "type": pc.localDescription.type,
        "sdp": pc.localDescription.sdp
    }))
    print("Sent complete answer to server!")

    # Wait for connection state to become connected
    print("Waiting for peer connection to establish...")
    for _ in range(30):
        if pc.connectionState == "connected":
            print("PeerConnection CONNECTED!")
            break
        await asyncio.sleep(0.1)

    print(f"Activating audio track and streaming for {duration} seconds...")
    track.active = True

    # Sample stats during active stream
    await asyncio.sleep(2.5)
    async with session.get(f"http://{target_ip}:8000/api/audio_stats") as resp:
        mid_stats = await resp.json()
        print("MID-STREAM STATS:")
        print(json.dumps(mid_stats, indent=2))

    await asyncio.sleep(duration - 2.5)

    # Test Talk OFF via DataChannel if open
    if dc.readyState == "open":
        print("Sending talk:0 via DataChannel...")
        dc.send("talk:0")
        await asyncio.sleep(0.5)

    print("Closing connection...")
    await ws.close()
    await session.close()
    await pc.close()
    print("Test finished successfully!")

if __name__ == "__main__":
    asyncio.run(run_test())
