# MaixCAM-CDK Phase 2 Performance Optimization — Executive Summary

## 1. End-to-End Comparison

| Metric | Original | Phase 1 | Phase 2 (`perf_probe` 8s) | Phase 2 (30-min Sustained) |
|---|---:|---:|---:|---:|
| Full-duplex system CPU | 97.7% | 87.5% | **81.5%** | **73.40% avg** (71.1–76.1%) |
| CPU idle headroom | 2.3% | 12.5% | **18.5%** | **26.60% avg** |
| Kernel logging CPU (`klogd+syslogd`) | 10.4–15.8% | 6.0–8.3% | **0.0%** | **0.0%** |
| `aic_btsdio` log rate | 533.3/s | 533.3/s | **0/s** | **0/s** (0 new in 30 min) |
| Audio uplink Opus encoding | 10,460 µs | 1,151 µs | **844 µs** | — |
| `_media_mutex` wait | 9,451 ms | 23.15 ms | **7.23 ms** | — |
| Video FPS (browser `disp_fps`) | ~29.5 baseline | ~24.8 | **29.16 fps** | **29.57 fps** (578,060 pkts, 0 lost) |
| Audio uplink/downlink rate | — | ~50 pkt/s | **50.0 / 50.0 pkt/s** | **50.05 / 50.05 pkt/s** (0 underruns) |

## 2. Key Findings

1. **Kernel Logging (`01-kernel-logging.md`)**: `aic_btsdio: skb type 3` and `aic_btsdio: hci0*` originated from unconditional `AICBT_INFO()` calls in `aic8800_fdrv/btsdio.c` on normal 8 kHz SCO HCI traffic (533.3/s). Gating `AICBT_INFO` / `AICBT_DBG` on `aicwf_dbg_level` eliminated 8.3% `klogd+syslogd` CPU overhead without suppressing warnings or errors.
2. **Video FPS Recovery (`02-video-fps.md`)**: The Phase 1 FPS drop (`29.6 -> 24.8–27.0 fps`) was caused by single-core CPU preemption (`cam_push nvcsw` 63/s -> 477/s) stretching synchronous `video_frame_push()` past the 33.3 ms deadline. Eliminating kernel logging + overlapping VENC/VI release with RTP packetization + `-O3` recovered video to **29.57 fps** with **0 packet loss**.
3. **Wi-Fi/Bluetooth Coexistence (`04-wifi-bluetooth.md`)**: Both 5 GHz STA (`5240 MHz`) and 2.4 GHz STA (`2462 MHz`) sustained 0% packet loss and ~29.2 fps alongside 2.4 GHz BT SCO, with 5 GHz exhibiting lower video jitter (`5.00 ms` vs. `6.00 ms`) and lower jitter-buffer delay (`49.60 ms` vs. `56.21 ms`).
4. **30-Minute Stability (`05-stability-test.md`)**: Sustained **73.40% avg CPU**, **29.57 fps**, **0 ALSA underruns**, **0 lost packets**, **0 new `aic_btsdio` logs**, and stable memory (`VmRSS` `23.74–24.14 MB`), followed by clean disconnect and reconnect.

## 3. Report Index

- [`01-kernel-logging.md`](01-kernel-logging.md) — `aic_btsdio` root cause, rates, minimal patch, before/after CPU
- [`02-video-fps.md`](02-video-fps.md) — Video pipeline instrumentation & FPS recovery
- [`03-cpu-profile.md`](03-cpu-profile.md) — Thread & process CPU breakdown
- [`04-wifi-bluetooth.md`](04-wifi-bluetooth.md) — USB vs. 5 GHz STA vs. 2.4 GHz STA coexistence
- [`05-stability-test.md`](05-stability-test.md) — 30-minute hardware stability test & reconnect check
- [`06-latency.md`](06-latency.md) — Audio/video pipeline latency & jitter breakdown
- [`07-optimizations.md`](07-optimizations.md) — Implemented kernel & C++ optimizations
- [`08-validation.md`](08-validation.md) — Acceptance matrix & rollback verification
- [`patches/aic8800_fdrv_gate_aicbt_info.patch`](patches/aic8800_fdrv_gate_aicbt_info.patch) — Minimal kernel driver patch
- [`data/stability_30m.csv`](data/stability_30m.csv) — 180-sample 30-minute CSV telemetry
