# Acceptance snapshot

- [x] MaixCAM creates its own AP automatically at boot; no PC AP/server.
- [x] AP clients associate and obtain DHCP addresses in the required range.
- [x] Entire UI and signaling served by MaixCAM without CDNs/public STUN.
- [x] HTTPS/WSS with private CA; operator confirms browser trust without warnings.
- [x] Operator confirms video and bidirectional audio over the AP.
- [x] Original Talk click-toggle preserved per explicit operator correction; no Hold mode added in final build.
- [x] Audio filters/backend, camera capture/encoder, orientation/rotation code preserved.
- [x] Existing A2DP target/headset strategy preserved; targeted reconnect and operator audio confirmation.
- [x] WSS reconnect, ownership, idle cleanup and Talk mute verified.
- [x] Software reboot, AP daemon recovery, app restart and single media process verified.
- [x] No router, PC server or Internet dependency in normal runtime.
- [x] Baseline/idle resource measurements, deployment backups, rollback and final handoff retained.
- [ ] Controlled full-load FPS, loss/jitter, sync, CPU/RAM and dropout/soak measurements.
- [ ] Distinct manual orientation/rotation and Wi-Fi client interruption acceptance reports.
- [ ] HFP audio path verification (only A2DP source found in authoritative baseline).

The initial WPA2 minimum was explicitly superseded by the operator's request for an open AP. HTTPS/WSS/WebRTC transport security was retained. Nonzero playback queue overflow counters were observed and remain a stability follow-up; do not infer dropout-free playback from the operator's functional confirmation.
