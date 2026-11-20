# PC to earbuds diagnosis — 2026-11-10

Unresolved: isolated native SCO playback is silent despite byte-exact source PCM at the Linux HCI boundary. No application or persistent configuration changes made.

## Measured results

- Initial AIC SDIO driver errors: `cmd queue crashed`, `bt send no cfm`, controller command timeout, stalled RX counters. Reboot cleared the observed stall but did not restore audible SCO. Allow startup time: the early five-second SSH failure did not establish device failure.
- After reboot: voice 0x0060 (signed 16-bit linear PCM / CVSD); SCO playback 8 kHz mono; playback and capture mixers unmuted at 15/15. RSSI -49 dBm, link quality 206, Wi-Fi AP channel 36 / 5180 MHz.
- Post-reboot 40-second WebRTC media test passed its media assertions, but user reported silent tone. Counters do not establish audibility.
- Direct SCO followed by A2DP: user reported fragmented sound, unsure which profile. A2DP playback stalled and was terminated. This is not a confirmed A2DP audible pass.
- Separate direct SCO-only test, application and microphone capture stopped: user reported silence.
- Isolated 15-second source: 440 Hz, 8000 Hz mono S16_LE, peak 12000. HCI TX capture: 944 packets of 254 payload bytes over 14.970 seconds; 239776 PCM bytes / 14.986 seconds; median gap 15.875 ms, max 31.844 ms, no gap above 40 ms. Captured samples exactly match source prefix. Final 224 source bytes were not captured.
- HCI RX: 2039 packets; status 2 (no data received) 1727, status 1 (possibly invalid) 212, status 0 (valid) 100. All status 1/2 payloads zero. This indicates link/controller trouble without proving its specific cause.
- Negotiated CVSD eSCO: voice 0x0060, interval 0x0c, retransmission window 0x02, TX/RX air packet lengths 60. Air packet sizes cannot be equated directly to host PCM packet sizes.

## Source check and next boundary

Sipeed LicheeRV-Nano-Build commit a4d8622406de75bc35c4e01207f23e9497bff5f0: btsdio.c forwards HCI to rwnx_sdio_bt_send_req; rwnx_msg_tx.c synchronously requests firmware confirmation; lmac_msg.h provides a 1024-byte Bluetooth array. The observed packet fits. Source was not verified to match the running driver binary exactly. Checkout outside repo: /home/ydt/maixcam-build/licheerv-driver-source.

Next: documented vendor SCO-over-SDIO configuration, controller/link handling, or a known-good headset/controller comparison. No evidence supports another application sample-rate change. A2DP-only audibility still needs verification; it does not provide the current duplex hands-free behavior.

Service restored to READY and diagnostic playback ended. Raw HCI traces are private microphone-containing artifacts and should be removed after analysis.
