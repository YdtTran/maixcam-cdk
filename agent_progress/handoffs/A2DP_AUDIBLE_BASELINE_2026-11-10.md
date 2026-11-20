# A2DP audible baseline — 2026-11-10

Status: AUDIBLE BASELINE CONFIRMED, with degraded clarity and incomplete playback.

Listener reported: “i can hear to số 6, it is not really clear, but yes hearable.” This establishes audible sound through A2DP on the same host, earbuds, and controller; it does not establish stable or clear playback.

- Branch: `feat/a2dp-audible-baseline`.
- Same host: `maixcam-ac10`, accessed through USB SSH `10.172.16.1`; Wi-Fi SSH `192.168.1.8` timed out.
- Same SDIO controller: `A8:B5:8E:22:37:55`.
- Same paired earbuds: EDIFIER X2 Plus, `6C:16:29:7B:00:D4`.
- Existing fixture: `/root/maixcam-tests/bluetooth-a2dp/count-60s.wav`, PCM S16_LE, stereo, 48000 Hz.
- Paused the media supervisor and service without stopping networking; disconnected the remote Handsfree profile. `bluealsa-aplay -L` then showed only A2DP playback.
- Playback command: `aplay -v -d 15 -D bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=a2dp /root/maixcam-tests/bluetooth-a2dp/count-60s.wav`.
- ALSA confirmed the BlueALSA A2DP sink, SBC codec, stereo 48000 Hz. This proves endpoint selection, not audibility.
- First actual playback attempt lost its SSH session; subsequent uptime reset proved a reboot occurred. Cause unknown; user said they did not reset the device.
- Second playback attempt retained boot ID `3c1f0b0a-8a57-4d9d-8d1d-f27d2521235b`, but did not finish by the 20-second watchdog. SIGTERM interrupted `pcm_write`; player exited 1. The watchdog result alone does not establish audibility; the listener separately confirmed counting through number 6.
- Restored media and supervisor; verified `STATE=READY`, media PID 1125, and no diagnostic aplay remaining.
- No application code, persistent Bluetooth configuration, or controller identity changed. No microphone trace captured.

The requested cross-profile audible baseline is established. A2DP clarity/stability and the original SCO silence remain unresolved. This result does not isolate the SCO fault to software, firmware, controller, or earbuds.
