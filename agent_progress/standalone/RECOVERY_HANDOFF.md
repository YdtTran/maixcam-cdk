# Network, boot and recovery milestone (B/G/H)

Objective: automatically start the camera-owned AP and local services without a router; preserve USB recovery and the Bluetooth manager.

Status: **PASS for AP/DHCP/DNS startup, tested fallback, reboot and daemon recovery. PARTIAL for sustained full-load coexistence.**

Inspected: device interfaces/routes, hostapd/dnsmasq, factory S30wifi, network and USB init scripts, original S99/service/autostart and Bluetooth manager. Changed: `target_services/standalone/{S30wifi,S99webrtc_autostart,network.sh,supervisor.sh}`, `scripts/standalone/{provision.py,deploy.py,apply.sh}`. USB, SSH, drivers, firmware, OS and Bluetooth scripts were not changed.

Actual architecture: wlan0 AP `MaixCAM`, 192.168.4.1/24, DHCP 192.168.4.100–150, local DNS maixcam.home.arpa → 192.168.4.1. Vietnam, preferred channel 36/5 GHz/20 MHz; channel 1/2.4 GHz fallback after primary startup failure. No NAT, router, external DNS, public ICE server or PC signaling server is required. The user explicitly changed AP security to OPEN; WPA2-PSK/CCMP remains the provisioning script's default when --open is omitted. This change did not weaken HTTPS/WSS/DTLS-SRTP.

Commands: safe deployment via USB; `test_recovery.sh`, `test_bluetooth_recovery.sh`, `test_fallback.sh`, `test_reboot.py`, `diagnose.sh`, host CPU summary, read-only hostapd/station/lease/BlueZ checks. Target test scripts/evidence are persistent under `/root/maixcam-tests/standalone-web`.

Evidence/results:

- `ap-client-status.txt`: actual AP stations and DHCP leases 192.168.4.119 (YdtTran) and .120; associated client .120 showed -38 dBm, 58.5/65 Mbps reported link rates and zero reported TX failures in that snapshot. This is a brief link sample, not a sustained throughput test.
- `fallback-test.txt`: primary failed before RF startup using a deliberately missing test interface; configured fallback enabled 2412 MHz/channel 1 with unchanged media PID and connected headset. Original config was restored; final-health confirms channel 36.
- `recovery-test.txt`: repeated starts retained the same AP/DNS/media PIDs. Graceful hostapd interruption recovered AP/DHCP/DNS within 10 seconds without replacing the media process. Media-service stop recovered within 8 seconds with one media PID.
- `bluetooth-recovery.txt`: existing targeted, non-scanning manager reconnected the known EDIFIER within 4 seconds; AP/media PID unchanged; BlueALSA A2DP endpoint returned. A later state check confirmed CONNECTED. This does not alone prove audible playback.
- `reboot.json`: software reboot changed boot ID from 661945d8-c813-4813-a328-c3dccdf4e99e to 85b94cc3-7b45-48f9-a77f-90908d091a15; USB observed READY after 65.18 seconds, with AP, DHCP/DNS, one native media process and Bluetooth connected. That timing includes USB/host observation delay. Boot services are unchanged in the final subsequent UI-only release.
- `operator-confirmation.md`: user subsequently confirmed functional video and bidirectional audio on the original-Talk-toggle build.

Rollback: every release backs up present/absent managed paths and original network inputs before mutation. Latest release is `/root/maixcam-standalone-20261101-030436-a285a4aa`; its rollback restores the preceding version. Original-router rollback: `ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-024022-47e9b94c/rollback.sh'`. Factory STA credentials/flags and baseline executable remain intact. Baseline SHA-256 dbc0602338e549072f1a886b0fa8557cba23ff61f27618ded25d610b9fcded7e matches the original tracked executable and the retained device baseline.

Observed final-session playback queue overflow counters are retained in final-operator-counters.json; no dropout-free coexistence claim is made. Band selection already uses 5 GHz, the PCM queue is bounded, and no buffer/gain/codec tuning was applied without a controlled comparison.

Remaining: controlled long-duration video/audio/AP/Bluetooth coexistence, loss/dropout/FPS measurements, physical power-cycle coverage and independent regulatory/RF power measurements. HFP capability is advertised by the headset but an active HFP pipeline was not found in the audited runtime. No audio/backend tuning was undertaken to mask these limits.
