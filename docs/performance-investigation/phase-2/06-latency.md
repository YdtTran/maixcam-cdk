# Investigation F — Audio & Video Latency Evaluation

## 1. Measurement Boundaries

We measured internal pipeline processing durations, RTP jitter, browser jitter-buffer (`jb_ms`), and browser decode times (`dec_ms`) directly from device instrumentation (`/api/perf_stats`, `/api/audio_stats`) and Chromium `RTCPeerConnection.getStats()`.

**Hardware boundary limitation**: True glass-to-glass video latency (photon-to-LCD) and acoustic mouth-to-ear audio latency require external photodiode/oscilloscope or acoustic loopback hardware, which was not attached. Those unmeasured segments (camera sensor exposure, OS compositor, earbud CVSD air/DAC delay) are noted explicitly below.

## 2. Video Latency & Jitter Breakdown (640x480 @ ~29 fps)

| Stage | USB RNDIS | 5 GHz STA (`5240 MHz`) | 2.4 GHz STA (`2462 MHz`) |
|---|---:|---:|---:|
| `venc_pop + venc_push + copy` | 1.37 ms | 1.26 ms | 1.07 ms |
| `video_frame_push()` (packetize + SRTP + `sendto`) | 15.61 ms | 15.15 ms | 14.75 ms |
| Browser RTP `jitter_ms` | 4.00 ms | 5.00 ms | 6.00 ms |
| Browser `jb_ms` (jitter buffer) | 31.84 ms | 49.60 ms | 56.21 ms |
| Browser `dec_ms` (H.264 decode) | 3.74 ms | 3.97 ms | 4.22 ms |
| **Measured Post-VI to Browser Decode** | **~52.6 ms** | **~70.0 ms** | **~76.3 ms** |

*Unmeasured external bounds*: ISP/VI sensor exposure + frame period (`~16–33 ms`) and browser/OS display compositor (`~16.7 ms` at 60 Hz).

## 3. Audio Uplink Latency & Jitter (BT Mic → Browser)

| Stage | USB RNDIS | 5 GHz STA (`5240 MHz`) | 2.4 GHz STA (`2462 MHz`) |
|---|---:|---:|---:|
| BT SCO 8 kHz capture frame pacing | 20.00 ms | 20.00 ms | 20.00 ms |
| Opus encode (8 kHz, complexity 1) | 0.84 ms | 0.88 ms | 0.82 ms |
| `audio_frame_push()` (SRTP + `sendto`) | 1.14 ms | 1.05 ms | 1.03 ms |
| Browser RTP `jitter_ms` | 3.00 ms | 4.00 ms | 3.00 ms |
| Browser `jb_ms` (jitter buffer) | 35.73 ms | 59.01 ms | 56.55 ms |
| **Measured Device-to-Browser Audio Uplink** | **~57.7 ms** | **~80.9 ms** | **~78.4 ms** |

*Unmeasured external bounds*: Earbud microphone ADC + CVSD air link (`~5–10 ms`).

## 4. Audio Downlink Latency (Browser → BT Earbuds)

| Stage | Measured Value |
|---|---|
| `G4c4OpusRx` Opus decode (8 kHz) | 0.79 ms |
| `G4c4BtPlayback` queue (`MAX_QUEUE_FRAMES = 25`, `FLUSH_KEEP_FRAMES = 2`) | 0–40 ms nominal (20–40 ms prebuffer) |
| `bt_playback` ALSA write pacing (`8000 frames/s`) | 12.3–20.0 ms/frame |
| **Measured Device Downlink Processing** | **~33–60 ms** |

*Unmeasured external bounds*: Browser `getUserMedia` + Opus encode + network RTT/2 (`~25–35 ms`) and earbud CVSD decode/DAC (`~5–10 ms`).
