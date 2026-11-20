# Investigation B — Video FPS Reduction & Recovery

## 1. Problem Statement

Phase 1 reported ~29.5 FPS in a baseline video-only/early test and ~24.8 FPS under full-duplex load. We instrumented both the device pipeline (`perf_probe.py`) and the browser WebRTC stack (`headless_client.mjs` CDP `RTCPeerConnection.getStats()` + `getVideoPlaybackQuality()`) to isolate the root cause.

## 2. Video-Only vs. Full-Duplex Comparison

| Metric | `InvB_VideoOnly` | `InvB_FullDuplex` (Pre-Patch) | `Opt1_KmodLog_FullDuplex` (Post-Kmod Patch) |
|---|---:|---:|---:|
| System Busy CPU | 58.0% | 90.6% | 82.8% |
| `vi_pop` avg/iter | 23.70 ms | 10.88 ms | — |
| `webrtc_push` avg/frame | 8.53 ms | 21.78 ms | 15.7 ms |
| `cam_push` `nvcsw` | 63/s | 477/s | 364/s |
| Video FPS (device sent) | 29.6 fps | 27.0 fps | 28.5 fps |

## 3. Root Cause

`_camera_push_thread()` CPU execution time per frame changed by only ~1 ms (`7.0 ms -> 8.0 ms`) between video-only and full-duplex. However, on the single-core RISC-V C906, adding full-duplex Bluetooth audio + `klogd`/`syslogd` logging increased involuntary context switches (`nvcsw`) in `cam_push` by **7.6x** (`63/s -> 477/s`).

This CPU preemption inflated the synchronous wall-clock duration of `video_frame_push()` (`8.53 ms -> 21.78 ms`), pushing the loop iteration (`vi_pop + venc + webrtc_push`) past the 33.3 ms frame deadline and dropping effective capture/encode FPS from 29.6 to 27.0 fps.

## 4. Recovery After Phase 2 Optimizations

1. **Eliminating `aic_btsdio` kernel logging** (`Opt1`): dropped `webrtc_push` wall time from `21.78 ms` to `15.7 ms` and recovered device FPS to `28.5 fps`.
2. **Reordering VENC/VI release before `video_frame_push()`** (`Opt3` in `src/maix_webrtc_maixcam.cpp`): frees VI/VENC buffers immediately so hardware ISP/VENC can pipeline the next frame concurrently with RTP packetization/SRTP encryption. Reduced `venc_pop` (`500 us -> 222.1 us`) and `venc_push` (`2306 us -> 1347 us`).
3. **Combined `Opt3 + Opt4 (1360B RTP MTU) + Opt5 (Opus complexity 1, fixed RX buffer) + Opt6 (-O3)`**:
   - Device `sent`: **28.8 fps** (`webrtc_push = 15.61 ms`, `venc_pop = 191.2 us`, `venc_push = 1154.4 us`)
   - Browser `RTCPeerConnection`: **29.19 rx_fps / 29.16 dec_fps / 29.16 disp_fps**, `rtp_drop = 0`, `pkts_lost = 0`, `jitter_ms = 4.00`, `jb_ms = 31.84 ms`, `dec_ms = 3.74 ms`.
