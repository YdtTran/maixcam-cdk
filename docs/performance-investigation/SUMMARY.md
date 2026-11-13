# Performance Investigation Summary — MaixCAM WebRTC + Bluetooth

## Executive Summary

On the single-core RISC-V Sipeed MaixCAM Lite (`nproc=1`, `rv64imafdvcsu`, 128 MB RAM), full-duplex operation (640x480 H.264 video + Bluetooth HFP/SCO 8 kHz CVSD mic uplink + browser-to-earbuds Talk downlink over standalone 5 GHz Wi-Fi AP) previously saturated the CPU at **97.7% busy (2.3% idle)**.

Through live hardware profiling (`scripts/perf_probe.py`), lightweight in-process instrumentation (`src/g4c4_perf.hpp`, `/api/perf_stats`), and subsystem isolation tests (`MAIX_DIAG_*`), we proved the saturation was **not** caused by camera busy-polling, `bt_playback`, `G4c4Resampler`, or `opus_rx`, but by four concrete bottlenecks:

1. **`_media_mutex` contention across `track->send()`**: Held for `13,893 ms` and waited on for `9,451 ms` (`50.1%` of an `18.87 s` window).
2. **48 kHz ALSA resampling + 48 kHz Opus complexity 5 + 5 ms polling in `opus_mic`**: Consumed `13.8–15.6%` CPU (`10,460 us` per 20 ms encode, `115–249 empty_reads/s`).
3. **`aic8800_fdrv` / `aic_btsdio` kernel debug log spam**: Drove `klogd + syslogd` to `10.4–15.8%` CPU.
4. **Size-optimized `-Os` build & idle 20 ms polling in `bt_playback`**.

## Key Results (Baseline vs Post-Optimization, Full Duplex)

| Metric | Baseline (`Test E`) | Post-Opt (`PostOpt_FullDuplex_Clean`) | Change |
|---|---:|---:|---:|
| **Total System CPU** | **97.7%** (`2.3%` idle) | **87.5%** (`12.5%` idle) | **-10.2%** |
| `stream_webrtc-m2` CPU | ~50.1% | **48.1%** (17s run: `38.9%`) | **-2.0 to -11.2%** |
| `_media_mutex` wait time | `9,451 ms` | `23.15 ms` | **-99.8%** |
| `opus_mic` CPU | `13.8–15.6%` | **7.8%** (17s run: `6.2%`) | **-6.0 to -9.4%** |
| `opus_enc` / 20 ms frame | `10,460 us` | `1,151.3 us` | **9.1x faster** |
| `opus_mic` empty reads/s | `115–249 /s` | `1.3 /s` | **-99%** |
| `klogd + syslogd` CPU | `10.4–15.8%` | **8.3%** (17s run: `6.0%`) | **-2.1 to -9.8%** |

## Deliverables

1. [`01-architecture.md`](01-architecture.md) — Complete process, thread, pipeline, and ownership map.
2. [`02-baseline.md`](02-baseline.md) — Unmodified baseline, CPU normalization (`nproc=1`), and mode matrix.
3. [`03-bottlenecks.md`](03-bottlenecks.md) — Ranked bottlenecks and ruled-out hypotheses.
4. [`04-experiments.md`](04-experiments.md) — Controlled isolation experiments (`Test A`–`Test E`, Opus complexity sweep).
5. [`05-optimizations.md`](05-optimizations.md) — Implemented changes and per-change measurements.
6. [`06-validation.md`](06-validation.md) — Post-optimization full-duplex validation on live hardware.
7. [`07-protocol-evaluation.md`](07-protocol-evaluation.md) — WebRTC transport cost evaluation and future headroom levers.
