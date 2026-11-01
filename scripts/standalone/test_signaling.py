#!/usr/bin/env python3
"""Trusted WSS ownership/reconnect/idle expiry; no media or audibility claims."""
import argparse
import asyncio
import json
import re
import ssl
import time
from pathlib import Path
import aiohttp

async def run(args):
    context = ssl.create_default_context(cafile=str(args.ca))
    base = f'https://{args.target}:8000'
    report = {}
    async with aiohttp.ClientSession(connector=aiohttp.TCPConnector(ssl=context)) as session:
        async with session.get(base + '/client.js') as response:
            token = re.search(r"const signalToken = '([a-f0-9]+)'", await response.text()).group(1)
        url = f'wss://{args.target}:8001/{token}/ownership-test'
        for cycle in range(3):
            async with session.ws_connect(url) as ws:
                await ws.send_json({'type': 'request', 'id': 'server'})
                offer = await ws.receive_json(timeout=15)
                assert offer['type'] == 'offer', offer
                candidates = [line for line in offer['sdp'].splitlines() if line.startswith('a=candidate:')]
                assert candidates and all(' typ host' in line for line in candidates)
                async with session.ws_connect(url + '-busy') as second:
                    assert (await second.receive_json(timeout=5))['type'] == 'busy'
                async with session.ws_connect(f'wss://{args.target}:8001/wrong-token/test') as invalid:
                    assert (await invalid.receive(timeout=5)).type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED)
                await ws.send_json({'type': 'ping'})
                assert (await ws.receive_json(timeout=5))['type'] == 'pong'
            await asyncio.sleep(1)
        report['trusted_wss_host_ice_reconnect_single_owner_invalid_token'] = 'PASS'
        async with session.ws_connect(url) as idle:
            async with session.post(base + '/api/talk?active=1') as response:
                assert response.status == 200
            started = time.monotonic()
            message = await idle.receive(timeout=42)
            assert message.type in (aiohttp.WSMsgType.CLOSE, aiohttp.WSMsgType.CLOSED), message
            report['idle_expiry_seconds'] = round(time.monotonic() - started, 2)
        await asyncio.sleep(1)
        async with session.get(base + '/api/audio_stats') as response:
            stats = await response.json()
            assert not stats['downlink']['talk_active'], stats
        report['disconnect_mutes_talk'] = 'PASS'
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', default='10.172.16.1')
    parser.add_argument('--ca', type=Path, default=Path('build/standalone-secrets/ca.crt'))
    parser.add_argument('--output', type=Path, default=Path('agent_progress/standalone/evidence/signaling.json'))
    asyncio.run(run(parser.parse_args()))
