# MaixCAM standalone operation

MaixCAM owns the Wi-Fi AP, DHCP, DNS, HTTPS UI, WSS signaling and existing DTLS-SRTP media pipeline. The PC/phone is a Wi-Fi client. Normal operation needs neither a PC server, router nor Internet. USB SSH remains available at `root@10.172.16.1` for recovery.

The current operator explicitly requested **an open Wi-Fi AP** on 2026-11-01, superseding the original WPA2 requirement. This is independent of TLS: HTTPS, WSS, browser certificate validation and WebRTC transport encryption remain enabled. Anyone in radio range can join the open AP and attempt to use the single media session. There is no separate web login.

## Connect a client

1. Join SSID **MaixCAM**, with **no Wi-Fi password** for the current deployment. Accept the OS option to remain connected without Internet. The preferred AP is 5 GHz channel 36, 20 MHz; the configured fallback is 2.4 GHz channel 1. A 2.4 GHz-only client does not trigger automatic fallback if the 5 GHz AP starts successfully; explicitly provision `--band 2.4` for such clients.
2. Confirm a DHCP address in `192.168.4.100`–`192.168.4.150`, subnet `/24`, DNS/gateway `192.168.4.1`. No NAT or Internet forwarding is provided.
3. Transfer **only** `build/standalone-secrets/ca.crt` to the client using a trusted channel. Verify its SHA-256 fingerprint against `openssl x509 -in build/standalone-secrets/ca.crt -noout -fingerprint -sha256` on the provisioning host. Never transfer `ca.key` to the camera or clients.
4. Install that certificate as a trusted CA:
   - Windows: import into the current user's **Trusted Root Certification Authorities**. For example, PowerShell: `Import-Certificate -FilePath .\ca.crt -CertStoreLocation Cert:\CurrentUser\Root`. Restart the browser.
   - macOS: import using Keychain Access and explicitly trust the root for SSL. Follow local administrator policy.
   - iPhone/iPad: install the CA profile, then enable full trust in Settings → General → About → Certificate Trust Settings.
   - Android: install a **CA certificate** in the device's certificate settings. Menu locations and enterprise restrictions vary. Confirm the chosen browser accepts the installed CA.
   - Firefox: if OS trust is not inherited, import into Settings → Privacy & Security → Certificates → Authorities and trust for websites.
5. Open **`https://192.168.4.1:8000/`** or **`https://maixcam.home.arpa:8000/`**. Both identities are included in the development certificate. DNS-over-HTTPS, VPNs or automatic network switching can prevent local DNS; use the IP URL or disable those features for this network as needed.
6. Confirm there is no certificate warning. Merely bypassing a warning is not the documented trust solution. Allow microphone permission. No insecure-origin flags are needed. Only one media session is admitted; other clients retry after receiving a busy response.
7. Use Listen for field audio. The original Talk button retains its click-to-toggle behavior. No Hold-to-Talk mode was added in the final implementation. Camera controls, rendering, orientation and manual rotation remain in the existing UI.

A successful Wi-Fi connection does not establish certificate trust. A trusted certificate does not verify Wi-Fi/media performance. These are separate acceptance checks.

## Build and provision

Use this checkout: `/home/ydt/maixcam-build/maixcam-webrtc-cockpit`. The older integration workspace and its history remain intact at `/home/ydt/maixcam-build/g4c4-webrtc-bluetooth-integration`.

The existing SDK, toolchain, base archive and metadata are required as described in the repository guidelines. Build with:

```sh
python3 scripts/update_http_server_html.py
python3 scripts/build.py
```

`src/client.js` is now the canonical signaling/UI client source. The previous generator contained older code than the deployed embedded JavaScript; that discrepancy was removed without reverting the working client behavior. Commit the regenerated header with frontend changes.

Generate an offline development identity and AP configuration on the host:

```sh
# Current operator-requested open AP:
python3 scripts/standalone/provision.py --country VN --band 5 --channel 36 --open
# Alternative WPA2-PSK/CCMP AP (secure default): omit --open.
# A random strong password is generated into ap-password.txt, or supply
# --password-file /a/private/file containing 16–63 printable ASCII characters.
```

Provisioning reuses the CA if present and issues a new leaf certificate. Preserve and protect the ignored `build/standalone-secrets` directory. The leaf lasts 365 days; the CA lasts 3650 days. Reissuing under the same CA does not require reinstalling the CA on existing clients. `ap-password.txt` is irrelevant when `ap-mode.txt` says OPEN; it can remain as the prior WPA2 recovery credential.

The country must match the actual operating location. The device's global regulatory domain is VN; its AIC8800 PHY reports a separate self-managed world domain. Channel 36 is permitted in the reported VN domain and was enabled by hostapd. Actual RF power/regulatory enforcement has not been independently measured. Do not interpret advertised PHY capabilities as verified AP/STA concurrency or client compatibility.

## Safe deployment

```sh
python3 scripts/standalone/deploy.py
# To deploy an already built binary:
python3 scripts/standalone/deploy.py --skip-build
```

Deployment deliberately uses **working USB SSH**, verifies RISC-V/musl ABI and binary SHA-256, stages libraries and embedded assets, checks the leaf/key, and backs up managed paths and original network inputs before changes. It retains the original service's media and Bluetooth environment. No USB/SSH init scripts, drivers, firmware, OS or audio gain/codec settings are changed.

A 180-second device-side rollback watchdog is armed before the service/network switch. The host commits health only after AP readiness, verified HTTPS/assets and verified WSS upgrade. Failure restores the immediately preceding configuration. Backups remain under `/root/maixcam-standalone-<timestamp>-<hash>/backup/`. Keep older release directories while they are rollback targets.

The factory S30wifi is retained as `/etc/init.d/S30wifi.pre-standalone`. The replacement S30wifi skips external-router STA startup when `/etc/maixcam-standalone/enabled` exists. The existing S99webrtc_autostart entry runs a single supervisor: AP/DHCP/DNS first, then the existing native service. No duplicate camera launcher is added. The supervisor checks process/network health every 10 seconds, with bounded AP startup, and retries failures without radio discovery. Bluetooth's existing event-driven manager is unchanged.

## Certificate rotation and production domain

For private-CA rotation, rerun the provision command with the same CA directory and desired AP mode, then deploy. The old leaf/key are backed up during deployment; the media/AP service restarts briefly. Clients keep trusting the existing CA. Do not delete the CA key unless intentionally migrating every client to a new authority.

For a publicly trusted deployment, provide a domain you control and a valid server certificate/full chain and private key. DNS-01 provisioning/renewal may require Internet and DNS-provider credentials on the provisioning host; normal camera operation remains offline. Place these as `server.crt` (leaf first, then intermediates), `server.key` and `ca.crt` (issuer trust bundle used by deployment verification) in a private configuration directory. Set the DNS record in `dnsmasq.conf` to that domain. Deploy with:

```sh
python3 scripts/standalone/deploy.py --config /private/config --tls-name camera.example.com
```

The TLS health check connects over USB while verifying the specified DNS identity. A public certificate normally will not cover the private IP or `home.arpa`; clients must use the certified domain. No publicly trusted domain/certificate was available during this implementation. Client CA installation remains a provisioning requirement for the delivered development identity; the operator confirmed it is trusted without browser warnings on their test client.

## Tests and diagnostics

Persistent target artifacts live under `/root/maixcam-tests/standalone-web`; persistent host evidence lives in `agent_progress/standalone/evidence`. Tests do not store results in `/tmp`.

```sh
python3 -m venv build/standalone-venv
build/standalone-venv/bin/pip install aiohttp aiortc av
build/standalone-venv/bin/python scripts/standalone/test_signaling.py
# Run this on a client with a working direct AP UDP route:
build/standalone-venv/bin/python scripts/standalone/test_device.py \
    --target 192.168.4.1 --cycles 2 --duration 30 \
    --output agent_progress/standalone/evidence/ap-media-test.json
```

The media test consumes video/audio, sends the existing sine track over browser-equivalent Opus, checks single-session ownership and records counters, decoded dimensions/FPS and RTP loss/jitter. It does not prove audible playback, orientation correctness or browser permissions. `aiortc` test compatibility normalizes the advertised H.264 profile exactly as the existing test did; the device encoder is unchanged. The native server and browser use host ICE candidates, without public STUN/TURN, for direct AP operation.

The implementation host is a VM whose USB recovery TCP path works but whose UDP echo probe did not reach the target. A failed WebRTC media attempt on that path is retained as FAIL, not interpreted as an AP/media success. Use an actual AP client for end-to-end validation.

Copy `scripts/standalone/diagnose.sh` to `/root/maixcam-tests/standalone-web/diagnose.sh` and run it with `sh` through USB. It records boot ID, AP/stations, ports, process counts, memory, two CPU samples and Bluetooth state. PSS is reported only if the kernel exposes it. `summarize_resources.py` computes CPU percentages from the captured jiffies without assuming HZ. The camera pipeline remains allocated while idle, so idle is not a zero-work camera state.

Other commands:

```sh
ssh root@10.172.16.1 '/etc/init.d/S99webrtc_autostart status'
ssh root@10.172.16.1 'tail -50 /root/maixcam-tests/standalone-web/supervisor.log'
ssh root@10.172.16.1 'hostapd_cli -p /run/maixcam-hostapd -i wlan0 status'
ssh root@10.172.16.1 'cat /root/maixcam-tests/standalone-web/dhcp.leases'
```

`test_recovery.sh` gracefully interrupts hostapd and the media service to exercise supervision. `test_bluetooth_recovery.sh` disconnects only the already paired headset and observes the existing targeted reconnect manager. Both are disruptive tests and must be run via USB. Neither continuously discovers Bluetooth devices.

If Wi-Fi fails, use USB. If USB also fails, stop device-dependent work and ask the operator to hard reset the camera. Do not assume it has recovered.

## Rollback

Use USB, even if the AP is broken:

```sh
ssh root@10.172.16.1 'cat /root/maixcam-tests/standalone-web/active-release'
# Substitute the recorded release directory:
ssh root@10.172.16.1 'sh /root/maixcam-standalone-<timestamp>-<hash>/rollback.sh'
```

Each rollback restores the configuration immediately before that release. To return all the way to the original router-based deployment, use the first successful standalone release's rollback:

```sh
ssh root@10.172.16.1 \
  'sh /root/maixcam-standalone-20261101-024022-47e9b94c/rollback.sh'
```

That stops standalone services, removes newly created managed paths, restores the original S30wifi/S99/service/daemon and restarts the saved STA/service configuration. Original `/boot/wifi.sta`, station credentials, Bluetooth manager and known-good media release were preserved. USB and SSH stay available. The original WLAN address may be reassigned by the router's DHCP server; verify it through USB rather than assuming `192.168.1.8`.

## Acceptance limits

Read `agent_progress/standalone/HANDOFF.md` for actual test outcomes. AP initialization, HTTPS/WSS health, boot/process recovery and controller-level Bluetooth reconnection are separate from AP-client DHCP, browser secure-context/microphone interaction and full media/coexistence testing. Do not mark the latter complete based on process existence or counters alone. The audited runtime advertises headset HFP capability but runs BlueALSA with A2DP source; no active HFP audio path was found or newly implemented.

Reference behavior: browser microphone access requires a [secure context (MDN)](https://developer.mozilla.org/en-US/docs/Web/API/MediaDevices/getUserMedia). The native WebSocket server's callback timing was checked against the [libdatachannel 0.24 server implementation](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.0/src/impl/websocketserver.cpp); pending public sockets must be retained until the TLS/HTTP handshake finishes.
