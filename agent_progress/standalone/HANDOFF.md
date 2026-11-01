# Standalone MaixCAM final handoff

Follow-ups RC-01/RC-02/RC-03 record the displayed rotation, provide real video preview, and retain recordings across F5/browser restart in IndexedDB; current release `/root/maixcam-standalone-20261101-034054-810730ff`. See [recording handoff](RECORDING_HANDOFF.md), [preview handoff](PREVIEW_HANDOFF.md), and [persistence handoff](PERSISTENCE_HANDOFF.md). The original acceptance/release details below remain historical.

The product implementation is deployed and running. MaixCAM itself hosts the AP, DHCP/DNS, complete embedded UI, HTTPS/WSS signaling and original native DTLS-SRTP media pipeline. The operator confirmed video and bidirectional audio. **Controlled long-duration quantitative acceptance is not claimed. Private-CA browser trust without warnings is operator-confirmed.**

## Final operator choices and access

- Country: Vietnam (VN).
- SSID: **MaixCAM**, **no Wi-Fi password**, explicitly requested by the operator. This supersedes the original WPA2 minimum for this deployment. Secure WPA2 remains the provisioning default when --open is omitted.
- Browser: **https://192.168.4.1:8000/** or **https://maixcam.home.arpa:8000/**.
- WSS: same certificate/host on port 8001; browser builds the URL automatically. WebRTC media remains DTLS-SRTP.
- Original click-to-toggle Talk behavior is preserved. The initially added Hold mode was removed after the operator clarified scope. Audio filtering, codec/gain settings, Bluetooth backend and inline orientation/rotation/layout code remain unchanged.
- Private CA certificate: `build/standalone-secrets/ca.crt` on provisioning host. Installation/trust is separate from Wi-Fi. Detailed client instructions and certificate rotation are in `docs/STANDALONE.md`.

## Deployment identity

Checkout: `/home/ydt/maixcam-build/maixcam-webrtc-cockpit` (original integration repository/history preserved).

Final binary SHA-256: `a285a4aa9335f59c3d7a9e248928585bea75b65008478c493d8f82aa3ad307f0`.

Device release: `/root/maixcam-standalone-20261101-030436-a285a4aa`.

Final health: READY, wlan0 AP channel 36, headset connected, native ports 8000/8001; checksum matches host build. See `evidence/final-health.txt` and `evidence/deploy-preserve-toggle.txt`.

## Phase outcomes

| Phase | Status and scope | Evidence |
|---|---|---|
| A | PASS: repository/device audit, ABI, working USB recovery established before mutation | AUDIT.md; audit-initial.txt; baseline-resource.txt |
| B | PASS: AP boot, both bands' initialization, fallback, actual client association/DHCP, local DNS | RECOVERY_HANDOFF.md; ap-client-status.txt; fallback-test.txt; reboot.json |
| C | PASS: complete on-device embedded assets, no CDN/font/adapter dependency | deploy-preserve-toggle.txt; ui-preservation.txt |
| D | PASS: trusted local WSS, host-only ICE, one owner, reconnect/idle cleanup; operator media confirmation | signaling.json; operator-confirmation.md |
| E | PASS: native TLS/private CA, trusted automated clients and operator-confirmed trusted browser without warnings | SIGNALING_HANDOFF.md; docs/STANDALONE.md |
| F | PARTIAL: original media/control code preserved; operator video/bidirectional audio PASS; detailed rotation/FPS/sync measurements pending | ui-preservation.txt; operator-confirmation.md |
| G | PARTIAL: AP plus connected Bluetooth and targeted reconnect PASS; controlled sustained full workload pending | bluetooth-recovery.txt; fallback-test.txt |
| H | PASS: software reboot, single launcher/media owner and process/AP recovery | reboot.json; recovery-test.txt |
| I | PARTIAL: baseline/idle resources measured; controlled active-load measurements pending | baseline-cpu.json; postboot-cpu.json; final-cpu.json; corresponding raw reports |
| J | PASS: reproducible build/deploy, backups/watchdog/rollback and final documentation | scripts/standalone; docs/STANDALONE.md; PLAN_INDEX.md |

## Source changes

`src/client.js` is the canonical working embedded client; the stale generator JavaScript was replaced with reading that source. HTTPS-aware WSS URLs, same-origin signaling token, host-only browser ICE, heartbeat/reload and explicit secure-context errors are added. The HTML drops external font/adapter requests. The original Talk onclick and notify/start/stop functions are retained; existing inline UI scripts/styles are byte-identical.

`src/maix_webrtc_http_server.hpp` embeds regenerated assets and configures native SSLServer, root/key paths, no-store/MIME headers, bounded I/O and HTTP Origin checks. `src/maix_webrtc_signaling_server.hpp` localizes the SDK signaling wrapper to add native TLS, pending-socket lifetime, bounded handshake/message size, single ownership and idle cleanup. `maix_webrtc_maixcam.cpp` selects empty native ICE server configuration only in standalone mode. `maix_webrtc_server.hpp` mutes Talk on disconnect and when a standalone RX session is recreated; the legacy decoder's default ON was exposed by a test. No decoder, filtering, resampling, hardware encoding, capture or Bluetooth backend files were modified.

`target_services/standalone` adapts the existing boot entry and supervisor; no duplicate media launcher. `scripts/standalone` provides provisioning, ABI/checksum/deployment health gates, rollback, persistent diagnostics and reproducible tests. Existing Bluetooth connection strategy and media service environment are retained. Build artifacts and generated header are updated.

## Resource observations

Linux sees 130868 kB (~128 MiB) RAM, no swap. Individual RSS values overlap shared library mappings and should not be summed as PSS.

| Sample | App RSS | Available RAM | App CPU | AP / DNS RSS |
|---|---:|---:|---:|---:|
| Original baseline | ~22 MiB | ~76 MiB | 56.06% over brief sample | not running |
| Post-boot allocated-camera idle | 20876 kB | ~80 MiB | 17.38% | 2016 / 764 kB |
| Final release idle snapshot | 21784 kB | ~71 MiB | 18.50% | 1988 / 812 kB |

The baseline workload was not controlled; these values do **not** establish a performance improvement. PSS is unavailable on this kernel. Full-load video FPS, loss/jitter, audio dropouts and long-run coexistence have not been quantitatively accepted. The timed final observation window had no active peer; its zero counters are not an active-load result. A later cumulative read shows operator-session activity but supplies no controlled active CPU/FPS/loss sample.

## Honest failures and limits

- The first archive attempt failed before mutations because target BusyBox tar lacks gzip support; deployment now uses plain tar.
- Early WSS guards closed sockets before TLS opened; callback timing/lifetime were corrected and trusted WSS retested successfully.
- Initial disconnect test found Talk reset to ON; standalone RX initialization was corrected and the test then passed.
- The VM's recovery TCP path works, but its UDP probe never reached MaixCAM. The automated media run on that path failed (zero frames) and remains recorded as FAIL in media-test-run3.txt. This is not claimed as an AP media failure or a successful automated media test. Actual AP clients associated and the operator separately confirmed video/audio.
- An opportunistic pre-final operator-session counter read showed playback activity and nonzero queue overflow/underrun history. The final build later reported 1011 uplink packets, 269760 playback frames, 294 playback queue overflows and zero underruns after the operator session (`final-operator-counters.json`); the peer was already closed at sampling time. The session was not controlled, and counters reset with peer/release changes; no dropout-free claim or gain/buffering change was made. Controlled coexistence validation remains necessary.
- The operator separately confirmed that the CA is trusted and the browser shows no certificate warning; this is recorded independently of microphone/video functionality. No public production domain/certificate credentials were provided. Private CA provisioning is delivered; normal runtime is offline.
- Device wall time returned to 1970 after offline reboot. Host UTC, boot ID and monotonic uptime identify test ordering; browser TLS checks use the client's clock. No NTP/router dependency was added to startup. RTC/time persistence and certificate renewal remain maintenance considerations.
- Headset HFP capability is advertised, but only active A2DP source was found. Existing backend was preserved; HFP audio was not invented or claimed verified.
- Manual rotation/orientation, long-duration sync/dropout testing, client Wi-Fi off/on recovery and physical power-cycle testing lack a distinct operator test report. Browser signaling reconnect, AP daemon recovery and software reboot were exercised.

## Rollback and next actions

Immediate previous version:

```sh
ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-030436-a285a4aa/rollback.sh'
```

Original router-based system and original media service:

```sh
ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-024022-47e9b94c/rollback.sh'
```

USB/SSH and original station credentials are preserved. If both Wi-Fi and USB fail, request an operator hard reset and stop device-dependent work until connectivity returns.

Next: run the retained AP media test on an actual AP client for controlled FPS/loss/jitter/dropouts/resource measurements; check rotation and repeated client/headset reconnects during full workload. Keep original audio/BT settings until measurements justify any change. Persistent target evidence: `/root/maixcam-tests/standalone-web`; host evidence: `agent_progress/standalone/evidence`. See SIGNALING_HANDOFF.md and RECOVERY_HANDOFF.md for milestone commands and evidence.
