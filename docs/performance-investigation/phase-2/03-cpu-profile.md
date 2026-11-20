# Investigation C — Full-Duplex CPU Profile

## 1. Measurement Methodology

CPU profiling was performed with `scripts/perf_probe.py` (8-second active sampling window, ~14s total probe window) during simultaneous 640x480 H.264 WebRTC video + 8 kHz Opus BT SCO mic uplink + 8 kHz Opus BT SCO earbud downlink (`scripts/headless_client.mjs --talk --listen`).

## 2. Final Optimized CPU Breakdown (`Opt6_O3_FullDuplex`)

- **System CPU**: `81.5%` busy (`41.7%` usr, `31.4%` sys, `0.0%` irq, `8.5%` sirq, `0.0%` iowait) — **18.5% idle headroom** (up from 9.4% Phase 2 baseline / 12.5% Phase 1 / 2.3% original).
- Note: `perf_probe.py` polling over USB SSH (`IRQ 41`, `449.8/s`) adds minor measurement overhead; during the 30-minute stability test with 10s sampling, idle headroom is slightly higher.

### Process Breakdown

| Process | CPU % | usr % | sys % | Role |
|---|---:|---:|---:|---|
| `stream_webrtc-m2` | 50.5% | 35.5% | 15.0% | WebRTC, ISP, Opus, ALSA |
| `bluealsa` | 3.1% | 0.6% | 2.5% | BlueZ SCO audio daemon |
| `kworker/0:0-events` | 2.2% | 0.0% | 2.2% | Kernel workqueues |
| `aicwf_bustx_thr` | 1.4% | 0.0% | 1.4% | AIC8800 SDIO TX thread |
| `kworker/0:1-events_long` | 1.3% | 0.0% | 1.3% | Kernel long workqueues |
| `kworker/u3:2-hci0` | 1.0% | 0.0% | 1.0% | Bluetooth HCI workqueue |
| `cvitask_vc_wt1` | 0.9% | 0.0% | 0.9% | Hardware H.264 VENC worker |
| `aicwf_busrx_thr` | 0.9% | 0.0% | 0.9% | AIC8800 SDIO RX thread |
| `cvitask_vpss_1` | 0.6% | 0.0% | 0.6% | Hardware VPSS scaler |

### `stream_webrtc-m2` Thread Breakdown

| Thread | CPU % | usr % | sys % | `vcsw/s` | `nvcsw/s` |
|---|---:|---:|---:|---:|---:|
| `cam_push` | 22.0% | 13.9% | 8.1% | 26 | 417 |
| `ISP0_RUN` | 10.6% | 8.7% | 1.9% | 60 | 3 |
| `opus_mic` | 7.1% | 5.1% | 2.0% | 64 | 31 |
| `opus_rx` | 3.9% | 3.5% | 0.4% | 49 | 12 |
| `juice poll` | 1.2% | 0.6% | 0.6% | 56 | 2 |
| `bt_playback` | 1.0% | 0.6% | 0.4% | 50 | 1 |
| `http_srv` | 0.6% | 0.4% | 0.1% | 89 | 2 |
| `pcm-io` (x2) | 1.0% | 0.5% | 0.4% | 183 | 1 |
| `RTC worker` (x4) | 1.9% | 1.6% | 0.3% | 56 | 4 |
| `stream_webrtc-m` + `SCTP timer` | 0.4% | 0.1% | 0.3% | 179 | 1 |

## 3. Dominant Remaining CPU Consumers

1. **`cam_push` (`22.0%`)**: Dominated by `libdatachannel` H.264 RTP packetization, SRTP encryption, and `sendto()` syscalls (`webrtc_push = 15.61 ms/frame`).
2. **`ISP0_RUN` (`10.6%`)**: Proprietary MaixCDK/CVI ISP pipeline running at `SCHED_FIFO` (`prio=-81`).
3. **`opus_mic` (`7.1%`) + `opus_rx` (`3.9%`)**: Opus 8 kHz complexity-1 encode (`844 us`) and decode (`793 us`) plus WebRTC audio SRTP send.
4. **SDIO + BT/USB SoftIRQ & Kernel (`~16-18%`)**: `IRQ 27 (mmc1)` at `1483.8/s` for BT SCO + Wi-Fi SDIO transport.
