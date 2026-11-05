#!/usr/bin/env python3
"""Build, stage and health-gate standalone mode through working USB recovery."""
import argparse
import hashlib
import re
import secrets
import shutil
import ssl
import socket
import base64
import os
import subprocess
import time
import http.client
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--target', default='root@10.172.16.1')
    parser.add_argument('--config', type=Path, default=ROOT / 'build/standalone-secrets')
    parser.add_argument('--tls-name', default='10.172.16.1', help='Certificate identity for health checks over USB')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_.@-]+', args.target):
        parser.error('target must be an SSH user@hostname or IPv4 address')
    def ssh(command, timeout=30):
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
                               args.target, command], capture_output=True,
                              text=True, timeout=timeout, check=False)
        if result.returncode:
            raise RuntimeError(f'SSH failed ({result.returncode}): {result.stdout} {result.stderr}')
        return result.stdout
    audit = ssh("uname -m; ip -4 addr show dev usb1; test -x /usr/sbin/hostapd; "
                "test -x /usr/sbin/dnsmasq; test -x /root/webrtc_service.sh")
    if 'riscv64' not in audit or '10.172.16.1/24' not in audit:
        raise SystemExit('Refusing network mutation: expected ABI and USB recovery not confirmed')
    # This deployment must actually arrive over USB, not merely observe usb1.
    if '10.172.16.1' not in args.target:
        raise SystemExit('Deploy over root@10.172.16.1 to prove the recovery channel')
    if not args.skip_build:
        subprocess.run(['python3', 'scripts/update_http_server_html.py'], cwd=ROOT, check=True)
        subprocess.run(['python3', 'scripts/build.py'], cwd=ROOT, check=True)
    binary = ROOT / 'build/stream_webrtc-m2'
    elf = subprocess.check_output(['readelf', '-h', '-l', str(binary)], text=True)
    if 'RISC-V' not in elf or 'ld-musl-riscv64' not in elf:
        raise SystemExit('Unexpected binary architecture or ABI')
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    shim_digest = hashlib.sha256((ROOT / 'build/dl_lib/libg4c4_sco_write_align.so').read_bytes()).hexdigest()
    stage = '/root/maixcam-standalone-' + time.strftime('%Y%m%d-%H%M%S') + '-' + digest[:8] + '-' + shim_digest[:8]
    local = ROOT / 'build/standalone-stage'
    if local.exists():
        shutil.rmtree(local)
    (local / 'config').mkdir(parents=True, mode=0o700)
    shutil.copytree(ROOT / 'target_services/standalone', local / 'services')
    for name in ['hostapd.conf', 'hostapd-fallback.conf', 'dnsmasq.conf', 'server.crt', 'server.key', 'ca.crt']:
        shutil.copy2(args.config / name, local / 'config' / name)
    (local / 'config/signal-token').write_text(secrets.token_hex(32) + '\n')
    shutil.copy2(ROOT / 'target_services/webrtc_service.sh', local / 'webrtc_service.sh')
    shutil.copy2(binary, local / binary.name)
    shutil.copytree(ROOT / 'build/dl_lib', local / 'dl_lib')
    shutil.copy2(ROOT / 'scripts/standalone/apply.sh', local / 'apply.sh')
    # Confirm identity and expiry before any service stop. Never ship the CA key.
    subprocess.run(['openssl', 'verify', '-CAfile', str(args.config / 'ca.crt'),
                    '-untrusted', str(args.config / 'server.crt'), str(args.config / 'server.crt')], check=True)
    cert_public = subprocess.check_output(['openssl', 'x509', '-in', str(args.config / 'server.crt'), '-pubkey', '-noout'])
    key_public = subprocess.check_output(['openssl', 'pkey', '-in', str(args.config / 'server.key'), '-pubout'])
    if cert_public != key_public:
        raise SystemExit('Certificate and private key do not match; no changes applied')
    ssh('umask 077; mkdir -p ' + stage)
    subprocess.run(['scp', '-rq', str(local) + '/.', args.target + ':' + stage + '/'],
                   check=True, timeout=120)
    remote_hash = ssh('sha256sum ' + stage + '/stream_webrtc-m2').split()[0]
    if remote_hash != digest:
        raise SystemExit('Binary transfer checksum mismatch; no changes applied')
    ssh('cp -p /root/webrtc_service.sh ' + stage + '/webrtc_service.original')
    print('STAGE=' + stage, flush=True)
    context = ssl.create_default_context(cafile=str(args.config / 'ca.crt'))
    def get_asset(path):
        # Validate the configured DNS certificate while transporting over USB.
        connection = http.client.HTTPSConnection(args.tls_name, 8000, timeout=10, context=context)
        connection._create_connection = lambda address, timeout, source_address=None: socket.create_connection(
            ('10.172.16.1', 8000), timeout, source_address)
        try:
            connection.request('GET', path)
            response = connection.getresponse()
            if response.status != 200:
                raise RuntimeError(f'HTTPS {path} status {response.status}')
            return response.read()
        finally:
            connection.close()
    try:
        print(ssh('sh ' + stage + '/apply.sh ' + stage, timeout=50), flush=True)
        for attempt in range(30):
            try:
                state = ssh('cat /run/webrtc_autostart.state; '
                            '/root/maixcam_standalone_network.sh status; '
                            'pidof stream_webrtc-m2', timeout=10)
                if 'STATE=READY' in state:
                    if b'talk-btn' not in get_asset('/'):
                        raise RuntimeError('Unexpected frontend')
                    js = get_asset('/client.js')
                    if b'__SIGNAL_TOKEN__' in js or b'iceServers: []' not in js:
                        raise RuntimeError('Unexpected signaling configuration')
                    token_match = re.search(rb"const signalToken = '([a-f0-9]+)'", js)
                    if not token_match:
                        raise RuntimeError('Missing same-origin signaling token')
                    with socket.create_connection(('10.172.16.1', 8001), timeout=10) as tcp:
                        with context.wrap_socket(tcp, server_hostname=args.tls_name) as tls:
                            key = base64.b64encode(os.urandom(16)).decode()
                            path = '/' + token_match[1].decode() + '/health'
                            request = (f'GET {path} HTTP/1.1\r\nHost: {args.tls_name}:8001\r\n'
                                       'Upgrade: websocket\r\nConnection: Upgrade\r\n'
                                       f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n')
                            tls.sendall(request.encode())
                            if b'101 Switching Protocols' not in tls.recv(4096):
                                raise RuntimeError('Trusted WSS upgrade failed')
                    break
            except (subprocess.SubprocessError, OSError, RuntimeError):
                pass
            time.sleep(2)
        else:
            raise RuntimeError('AP/native HTTPS health check timed out')
        print(ssh('touch ' + stage + '/health-committed; '
                  'echo ' + stage + ' > /root/maixcam-tests/standalone-web/active-release; '
                  'cat /run/webrtc_autostart.state; iw dev wlan0 info; free -m'), flush=True)
        print('DEPLOYED: https://maixcam.home.arpa:8000/ (client CA trust required)')
        print('ROLLBACK: ssh ' + args.target + " 'sh " + stage + "/rollback.sh'")
    except Exception:
        print(ssh('test ! -f ' + stage + '/rollback.sh || sh ' + stage + '/rollback.sh', timeout=45), flush=True)
        raise

if __name__ == '__main__':
    main()
