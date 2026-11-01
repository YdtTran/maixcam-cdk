#!/usr/bin/env python3
"""Create private offline CA, server identity and WPA2 AP configuration on host.
Secrets stay in the ignored output directory; only leaf identity goes to device.
"""
import argparse
import hashlib
import secrets
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=Path('build/standalone-secrets'))
    parser.add_argument('--domain', default='maixcam.home.arpa')
    parser.add_argument('--country', required=True)
    parser.add_argument('--ssid', default='MaixCAM')
    parser.add_argument('--open', action='store_true', help='Explicitly opt into an unencrypted Wi-Fi AP')
    parser.add_argument('--password-file', type=Path)
    parser.add_argument('--band', choices=['2.4', '5'], default='2.4')
    parser.add_argument('--channel', type=int)
    args = parser.parse_args()
    import re
    if not re.fullmatch(r'[A-Z]{2}', args.country):
        parser.error('country must be a two-letter uppercase ISO code')
    if not re.fullmatch(r'[a-z0-9]+(?:[.-][a-z0-9]+)*', args.domain):
        parser.error('invalid DNS hostname')
    if not 1 <= len(args.ssid.encode()) <= 32:
        parser.error('SSID must be 1–32 bytes')
    channel = args.channel or (36 if args.band == '5' else 1)
    if channel not in ([36, 40, 44, 48] if args.band == '5' else [1, 6, 11]):
        parser.error('use an explicitly validated non-DFS channel')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    out.chmod(0o700)
    password_path = out / 'ap-password.txt'
    password = (args.password_file.read_text().strip() if args.password_file else
                password_path.read_text().strip() if password_path.exists() else secrets.token_urlsafe(24))
    if not 16 <= len(password) <= 63 or not password.isascii() or not password.isprintable():
        parser.error('AP password must be 16–63 printable ASCII characters')
    password_path.write_text(password + '\n')
    password_path.chmod(0o600)
    def run(*cmd):
        subprocess.run(cmd, check=True, cwd=out, stdout=subprocess.DEVNULL)
    if not (out / 'ca.key').exists():
        run('openssl', 'req', '-x509', '-newkey', 'rsa:3072', '-nodes', '-sha256',
            '-days', '3650', '-subj', '/CN=MaixCAM Offline Root CA',
            '-addext', 'basicConstraints=critical,CA:TRUE,pathlen:0',
            '-addext', 'keyUsage=critical,keyCertSign,cRLSign',
            '-keyout', 'ca.key', '-out', 'ca.crt')
    run('openssl', 'req', '-new', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
        '-nodes', '-subj', '/CN=' + args.domain, '-keyout', 'server.key', '-out', 'server.csr')
    (out / 'server.ext').write_text(
        'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n'
        'extendedKeyUsage=serverAuth\nsubjectAltName=DNS:' + args.domain +
        ',IP:192.168.4.1,IP:10.172.16.1\n')
    run('openssl', 'x509', '-req', '-in', 'server.csr', '-CA', 'ca.crt', '-CAkey', 'ca.key',
        '-CAcreateserial', '-days', '365', '-sha256', '-extfile', 'server.ext', '-out', 'server.crt')
    psk = hashlib.pbkdf2_hmac('sha1', password.encode(), args.ssid.encode(), 4096, 32).hex()
    (out / 'hostapd.conf').write_text(
        'interface=wlan0\ndriver=nl80211\nctrl_interface=/run/maixcam-hostapd\n'
        f'ssid2={args.ssid.encode().hex()}\ncountry_code={args.country}\nieee80211d=1\n'
        f'hw_mode={"a" if args.band == "5" else "g"}\nchannel={channel}\n'
        'ieee80211n=1\nwmm_enabled=1\nmax_num_sta=8\nap_isolate=0\n'
        'auth_algs=1\nwpa=2\nwpa_key_mgmt=WPA-PSK\nrsn_pairwise=CCMP\n'
        f'wpa_psk={psk}\n')
    # Same credentials and country, fixed 2.4 GHz fallback without scanning.
    fallback = (out / 'hostapd.conf').read_text()
    fallback = re.sub(r'^hw_mode=.*$', 'hw_mode=g', fallback, flags=re.M)
    fallback = re.sub(r'^channel=.*$', 'channel=1', fallback, flags=re.M)
    (out / 'hostapd-fallback.conf').write_text(fallback)
    (out / 'hostapd-fallback.conf').chmod(0o600)
    (out / 'dnsmasq.conf').write_text(
        'interface=wlan0\nlisten-address=192.168.4.1\nbind-interfaces\n'
        'no-resolv\nno-hosts\ndomain-needed\nbogus-priv\n'
        f'local=/{args.domain}/\naddress=/{args.domain}/192.168.4.1\n'
        'dhcp-authoritative\ndhcp-range=192.168.4.100,192.168.4.150,255.255.255.0,12h\n'
        'dhcp-option=3,192.168.4.1\ndhcp-option=6,192.168.4.1\n'
        'dhcp-leasefile=/root/maixcam-tests/standalone-web/dhcp.leases\n'
        'pid-file=/run/maixcam-dnsmasq.pid\n')
    if args.open:
        for name in ['hostapd.conf', 'hostapd-fallback.conf']:
            config = (out / name).read_text()
            config = re.sub(r'^(wpa|wpa_key_mgmt|rsn_pairwise|wpa_psk)=.*\n', '', config, flags=re.M)
            (out / name).write_text(config + 'wpa=0\n')
        (out / 'ap-mode.txt').write_text('OPEN: no Wi-Fi password; explicitly requested by operator\n')
    else:
        (out / 'ap-mode.txt').write_text('WPA2-PSK/CCMP\n')
    for name in ['ca.key', 'server.key', 'hostapd.conf']:
        (out / name).chmod(0o600)
    print(f'Provisioned {out}; URL https://{args.domain}:8000/; install ca.crt on clients.')
    print('Open AP: no password.' if args.open else 'AP password is in ap-password.txt (not printed).')
    print('CA private key stays on host.')

if __name__ == '__main__':
    main()
