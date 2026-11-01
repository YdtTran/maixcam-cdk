# Phase A audit and implementation scope

Objective: verify the live source/device, preserve media, and establish recovery before network mutation.

Status: **PASS for audit and USB recovery prerequisites**. Advertised features are distinguished from exercised capabilities below. Raw evidence: `evidence/audit-initial.txt`, `evidence/baseline-resource.txt`, and deployment/recovery logs. No hardware media acceptance is implied.

The user-named integration directory exists and retains its own AGENTS.md, PLAN-00..15 index, logs, candidate code and baseline artifacts. The supplied working directory is the newer `maixcam-webrtc-cockpit` repository, commit `29b4c12`. Its source, stage name and service scripts match the live application: `/root/g4c4-m2-stage-20261031-dbc06023/stream_webrtc-m2`, PID 933 before changes. Work was applied to this checkout; the older integration workspace was not overwritten.

Inspected files: repository AGENTS.md and original integration AGENTS.md; `src/maix_webrtc_maixcam.cpp`, `maix_webrtc_server.hpp`, `maix_webrtc_http_server.hpp`, `index.html`, `g4c4_opus_rx.hpp`, `g4c4_bt_playback.hpp`, `g4c4_resampler.hpp`; `scripts/build.py`, `deploy.sh`, `update_http_server_html.py`, `test_webrtc_aiortc.py`; target S30wifi, S40network, S80dnsmasq, S99local, S99webrtc_autostart, existing service/autostart/Bluetooth manager; SDK signaling, rtc configuration and httplib headers; build metadata and shared library inventory.

Commands executed (bounded SSH): `uname -a`, `/proc/cpuinfo`, `/proc/meminfo`, `iw list`, `iw dev`, `iw reg get`, `ip addr`, `ip route`, `lsmod`, `free -m`, `ps w`, `hostapd -v`, `dnsmasq --version`, `openssl version`, `iptables -S`, `df -h`, individual process status/stat/smaps reads, `bluetoothctl info 6C:16:29:7B:00:D4`, and read-only startup/config inspection. Both `ssh root@192.168.1.8` and `ssh root@10.172.16.1` worked before mutation. USB1 had 10.172.16.1/24; USB0 had 10.172.17.1/24. No USB/SSH configuration was changed.

Findings:

- RISC-V Linux 5.10.4, musl. Linux-visible RAM: 130868 kB (~128 MiB), not the nominal 256 MB board capacity. No swap. Initial available memory ~76 MiB; initial application RSS ~22 MiB. PSS interfaces were unavailable.
- AIC8800 SDIO driver already loaded. AP mode, 2.4/5 GHz bands and AP/STA combinations are advertised by `iw list`. **Concurrency was not tested and is not used.** Driver/firmware/kernel were not changed.
- Original STA used wlan0 192.168.1.8 on channel 52. Existing S30wifi already supported an AP branch but had unsuitable generated credentials/addressing. Existing USB DHCP services remain independent.
- hostapd 2.10 and dnsmasq 2.89 are installed; native nl80211 AP startup subsequently succeeded on channel 36. Vietnam was explicitly supplied by the user. Global domain changed to VN through hostapd country configuration; the PHY continues reporting a self-managed world domain. Reported transmit power is 0 dBm; actual RF power was not measured.
- OpenSSL 3.1.4/shared libraries were already installed and linked by the application. Native httplib SSLServer and rtc WebSocketServer TLS avoid adding a proxy/runtime/process.
- Web UI and signaling already lived on the camera. No frontend bundler exists: HTML/JS are embedded C++ strings. Original external font/adapter URLs and public STUN were runtime Internet dependencies. The generator's JavaScript was older than the embedded client; the embedded working version was extracted to canonical `src/client.js` before changes.
- Existing UI was Talk-toggle, not Hold-to-Talk as described in the mission. A Hold mode was initially added, then removed after the operator explicitly clarified that Talk behavior must not change. The final UI retains the original toggle. Original inline UI scripts/styles (including rotation/recording/layout) remain byte-identical.
- The paired EDIFIER was connected. Runtime BlueALSA is `-p a2dp-source`; the known device exposes HFP capability, but no active HFP audio path was found in this checkout/runtime. No HFP implementation was invented or altered.
- Original autostart waited for STA association and exited after launch; it did not continuously supervise the media service. Its single S99 entry was adapted for AP readiness and ongoing recovery.
- The implementation host is a VM behind 192.168.249.2. TCP to USB works; a separate 12-second UDP echo server received no packet from this VM. WebRTC media over this recovery path failed and is retained honestly as FAIL. A directly attached AP client remains necessary.

Files changed: see HANDOFF.md. No g4c4 media/backend files, encoder settings, capture orientation or audio gain/codec settings were changed. Two minimal native media-wrapper changes select host-only ICE in standalone mode and initialize/reinitialize Talk muted after a demonstrated disconnect test failure.

Rollback: original factory S30wifi, S99, application service and daemon were backed up before changes. Full original-router rollback is `/root/maixcam-standalone-20261101-024022-47e9b94c/rollback.sh` through USB. All original station credentials/boot inputs and Bluetooth manager remain intact. Use the latest release rollback for only the preceding version.

Next steps: browser microphone/coexistence acceptance, client CA installation and audible headset verification. Direct AP associations and DHCP leases (.119/.120) were subsequently observed; see ap-client-status.txt. Do not claim these based on successful build or service startup.
