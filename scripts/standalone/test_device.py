#!/usr/bin/env python3
"""Trusted HTTPS/WSS + one-owner + WebRTC media test. No insecure TLS flags.
Run on an AP client to validate the radio path; USB runs validate services only.
"""
import argparse
import asyncio
import json
import logging
import re
import ssl
import sys
import time
from pathlib import Path

import aiohttp
from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_webrtc_aiortc import SineAudioTrack


async def run(args):
    logging.basicConfig(level=logging.INFO)
    context = ssl.create_default_context(cafile=str(args.ca))
    base = f'https://{args.target}:8000'
    report = {'target': args.target, 'path': 'AP client' if args.target == '192.168.4.1' else 'recovery network',
              'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'cycles': []}
    async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(ssl=context),
                                    timeout=aiohttp.ClientTimeout(total=20)) as session:
        async with session.get(base + '/') as response:
            assert response.status == 200
            html = await response.text()
            assert 'talk-btn' in html and 'adapter-latest.js' not in html and 'fonts.googleapis.com' not in html
            assert response.headers.get('Cache-Control') == 'no-store'
        async with session.get(base + '/client.js') as response:
            js = await response.text()
            token = re.search(r"const signalToken = '([a-f0-9]+)'", js).group(1)
            assert 'iceServers: []' in js
        async with session.post(base + '/api/talk?active=0', headers={'Origin': 'https://untrusted.invalid'}) as response:
            assert response.status == 403, response.status
        report['https_and_origin'] = 'PASS'
        ws_url = f'wss://{args.target}:8001/{token}/test'
        for cycle in range(args.cycles):
            peer = RTCPeerConnection(RTCConfiguration(iceServers=[]))
            tone = SineAudioTrack(frequency=440)
            tone.active = True
            frames = {'video': 0, 'audio': 0, 'dimensions': [], 'video_times': []}
            consumers = []
            @peer.on('track')
            def on_track(track):
                async def consume():
                    try:
                        while True:
                            frame = await track.recv()
                            frames[track.kind] += 1
                            if track.kind == 'video':
                                dims = [frame.width, frame.height]
                                if dims not in frames['dimensions']:
                                    frames['dimensions'].append(dims)
                                frames['video_times'].append(time.monotonic())
                    except Exception:
                        return
                consumers.append(asyncio.create_task(consume()))
            try:
                async with session.ws_connect(ws_url, heartbeat=5) as ws:
                    await ws.send_json({'id': 'server', 'type': 'request'})
                    message = await ws.receive_json(timeout=15)
                    assert message['type'] == 'offer', message
                    sdp = message['sdp']
                    candidates = [line for line in sdp.splitlines() if line.startswith('a=candidate:')]
                    assert candidates and all(' typ host' in line for line in candidates), candidates
                    # Existing test compatibility normalization; encoder is untouched.
                    args.output.with_suffix('.offer.sdp').write_text(sdp)
                    sdp = sdp.replace('4D0034', '42e01f').replace('4d0034', '42e01f')
                    await peer.setRemoteDescription(RTCSessionDescription(sdp=sdp, type='offer'))
                    for transceiver in peer.getTransceivers():
                        if transceiver.kind == 'audio':
                            transceiver.direction = 'sendrecv'
                            transceiver.sender.replaceTrack(tone)
                    await peer.setLocalDescription(await peer.createAnswer())
                    args.output.with_suffix('.answer.sdp').write_text(peer.localDescription.sdp)
                    await ws.send_json({'id': 'server', 'type': 'answer', 'sdp': peer.localDescription.sdp})
                    # A second socket must not reset/steal the first session.
                    async with session.ws_connect(ws_url + '-second') as second:
                        busy = await second.receive_json(timeout=5)
                        assert busy.get('type') == 'busy', busy
                    async def keepalive():
                        while True:
                            await ws.send_json({'type': 'ping'})
                            await asyncio.sleep(5)
                    heartbeat = asyncio.create_task(keepalive())
                    try:
                        # Same controls as the browser, no changes to audio settings.
                        async with session.post(base + '/api/talk?active=1') as response:
                            assert response.status == 200
                        await asyncio.sleep(args.duration)
                        async with session.get(base + '/api/audio_stats') as response:
                            metrics = await response.json()
                        stats = await peer.getStats()
                        inbound = [dict(id=s.id, kind=s.kind, packetsReceived=s.packetsReceived,
                                        packetsLost=s.packetsLost, jitter=s.jitter)
                                   for s in stats.values() if s.type == 'inbound-rtp']
                        times = frames.pop('video_times')
                        fps = (len(times) - 1) / (times[-1] - times[0]) if len(times) > 1 else 0
                        result = {'cycle': cycle + 1, 'connection_state': peer.connectionState,
                                  'frames': frames, 'decoded_video_fps': round(fps, 2),
                                  'inbound_rtp': inbound, 'device_metrics': metrics,
                                  'host_candidates': candidates, 'one_owner': 'PASS'}
                        print(json.dumps(result), flush=True)
                        report['cycles'].append(result)
                        assert peer.connectionState == 'connected' and frames['video'] > 0 and frames['audio'] > 0
                    finally:
                        heartbeat.cancel()
                        async with session.post(base + '/api/talk?active=0') as response:
                            await response.read()
            finally:
                for task in consumers:
                    task.cancel()
                await peer.close()
                tone.stop()
            await asyncio.sleep(2)
    report['result'] = 'PASS (audibility and browser UI require operator verification)'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('REPORT=' + str(args.output))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', default='10.172.16.1')
    parser.add_argument('--ca', type=Path, default=Path('build/standalone-secrets/ca.crt'))
    parser.add_argument('--duration', type=float, default=12)
    parser.add_argument('--cycles', type=int, default=2)
    parser.add_argument('--output', type=Path, default=Path('agent_progress/standalone/evidence/media-test.json'))
    asyncio.run(run(parser.parse_args()))
