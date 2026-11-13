# 02 — System Baseline & CPU Accounting

## 1. CPU Accounting Methodology

- **Core count**: `nproc = 1` (single-core `rv64imafdvcsu`).
- **htop / top normalization**: On a 1-core system, 100% total CPU = 100% of 1 core. Per-thread CPU percentages and process CPU percentages share the same 0–100% scale (with minor rounding/sampling differences between `/proc/stat`, `/proc/<pid>/task/*/stat` jiffies at `USER_HZ=100`, and `CLOCK_THREAD_CPUTIME_ID`).
- **Load average vs CPU%**: A load average of `6.8–7.6` on a 1-core CPU reflects multiple runnable/uninterruptible threads (`cam_push`, `opus_mic`, `ISP0_RUN`, `kworker`, `klogd`, `syslogd`) queued behind the single core and `SCHED_RR` `ISP0_RUN` / kernel I/O.
- **Measurement tool**: `scripts/perf_probe.py` samples `/proc/uptime`, `/proc/stat`, `/proc/<pid>/stat`, `/proc/<pid>/task/*/stat`, `/proc/<pid>/task/*/status`, `/proc/interrupts`, `/api/audio_stats`, and `/api/perf_stats` over steady-state windows while `scripts/headless_client.mjs` drives a real Chromium WebRTC peer.

## 2. Baseline Measurements by Operating Mode

| Mode / Test | Total SYS CPU | `usr` / `sys` / `sirq` | `stream_webrtc-m2` | Top Contributors |
|---|---:|---:|---:|---|
| **Idle (No browser, BT connected)** | **53.1%** | 27.8 / 22.1 / 3.2% | 14.7% | `ISP0_RUN` 10.9%, `klogd+syslogd` 5.4%, `opus_mic` 2.0%, `bluealsa` 1.4%, `aicwf` 1.1% |
| **Test A: Video only** (`MAIX_DIAG_DISABLE_BT_CAPTURE=1`, Talk OFF) | **60.6%** | 33.2 / 25.3 / 2.1% | 35.7% | `cam_push` 23.5% (`31.9%` thread_cpu), `ISP0_RUN` 10.3%, `http_srv` 0.6% |
| **Test B: Video + Opus RX (no decode)** | **64.9%** | 36.0 / 24.8 / 4.0% | 37.5% | Test A + `juice poll` 1.2%, `RTC worker` ~1.8%, `sirq` +1.9% |
| **Test C: Video + Opus RX + Opus Decode (no BT playback)** | **66.3%** | 39.7 / 21.8 / 4.7% | 41.5% | Test B + `opus_rx` **3.0%** (`782.6 us/pkt` at 50 pkt/s) |
| **Test D: Video + Local PCM BT Playback (no Opus RX)** | **89.2%** | 45.5 / 40.7 / 3.0% | 40.5% | Test A + `klogd+syslogd` **15.8%** (8.1%+7.7%), `kworker` 5.7%, `aicwf` 3.3%, `bluealsa` 2.8%, `bt_playback` **1.2%** |
| **Baseline Video + BT Mic Uplink (Listen ON, Talk OFF)** | **84.2–85.8%** | 50.7 / 28.8 / 4.7% | 42.9–51.2% | `cam_push` 21.3–24.2%, `opus_mic` 13.8–15.6%, `ISP0_RUN` 10.5–10.7%, `klogd+syslogd` 4.8–5.1% |
| **Test E: Full Duplex (Video + BT Mic + Talk ON BT Playback)** | **97.7%** | 56.1 / 33.2 / 8.4% | 35.2% (throttled by contention) | `cam_push` 14.7% (`22.9%` thread_cpu), `ISP0_RUN` 10.5%, `opus_mic` 9.1% (`14.1%` thread_cpu), `klogd+syslogd` **10.4%**, `bluealsa` 2.1%, `kworker` 3.0% |

## 3. Opus Uplink Complexity Sweep (48 kHz Mono, Video + BT Mic)

| `MAIX_DIAG_OPUS_COMPLEXITY` | `opus_encode()` avg us/frame | `opus_mic` CPU (`/proc` / `thread_cpu`) | Total SYS CPU |
|---:|---:|---:|---:|
| **1** | 2,391.4 us | 12.0% / 11.1% | 81.5% |
| **3** | 3,109.4 us | 13.5% / 12.9% | 83.0% |
| **5** (baseline) | 4,140.6 us | 13.8% / 14.1% | 84.2% |

## 4. Meaning of "Video + Earbuds"

From runtime configuration (`target_services/webrtc_service.sh` sets `MAIX_WEBRTC_AUDIO_SOURCE=mic` and `MAIX_WEBRTC_BT_DEVICE=bluealsa:DEV=6C:16:29:7B:00:D4,PROFILE=sco`) and `/api/audio_stats`:
- In standalone mode, **Talk defaults to OFF** (`talk_active: false`), while **BT microphone capture + Opus uplink runs continuously** whenever a peer is connected.
- Therefore, "Video + earbuds" corresponds to **Video + BT Mic Uplink (79–86% CPU)**, whereas "All features simultaneously" corresponds to **Test E Full Duplex with Talk ON (~97.7% CPU)**.
