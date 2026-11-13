# Investigation E — 30-Minute Hardware Stability Test

## 1. Test Configuration

- **Tool**: [`scripts/stability_30m.py`](../../../scripts/stability_30m.py) running [`scripts/headless_client.mjs`](../../../scripts/headless_client.mjs) (`--talk --listen --duration 1820`, Chromium fake media device generating continuous 1 kHz audio + animated video).
- **Sampling**: 180 samples at 10-second intervals (`1800s` total), recorded to [`data/stability_30m.csv`](data/stability_30m.csv).
- **Binary**: `stream_webrtc-m2` (`106923c8`), PID `720` throughout the entire run (0 restarts).

## 2. 30-Minute Time-Series Summary (180 Samples)

| Metric | Minimum | Average | Maximum | Target / Expectation |
|---|---:|---:|---:|---|
| System Busy CPU | 71.10% | **73.40%** | 76.10% | <= 80% |
| Device Video FPS (`frames_sent/dt`) | 28.00 fps | **29.60 fps** | 36.30 fps | ~30 fps |
| Audio Uplink Rate | 48.10 pkt/s | **50.05 pkt/s** | 62.00 pkt/s | 50.0 pkt/s |
| Audio Downlink Decode Rate | 48.00 pkt/s | **50.05 pkt/s** | 61.90 pkt/s | 50.0 pkt/s |
| `stream_webrtc-m2` `VmRSS` | 23.74 MB | 23.98 MB | 24.14 MB | No memory leak |
| System `MemAvailable` | 74.94 MB | 78.27 MB | 79.40 MB | Stable headroom |
| SoC Thermal (`thermal_zone0`) | 99.30 °C | 102.69 °C | 104.20 °C | Plateaued, no crash |

## 3. Error & Integrity Counters Over 30 Minutes

- **ALSA Playback Underruns**: `0`
- **ALSA Errors**: `0`
- **Opus Decode Errors**: `0`
- **`aic_btsdio` `dmesg` count**: `95` at start → `95` at end (**0 new messages** in 30 minutes)
- **Kernel `cmd timed-out` / `bt send no cfm` / `OOM` errors**: `0`

## 4. Browser End-of-Run & Reconnect Verification

- **30-Minute Browser Session (`dt = 1820.10 s`)**:
  - `BROWSER_VIDEO`: `rx_fps=29.57`, `dec_fps=29.57`, `disp_fps=29.57`, `rtp_drop=0`, `pkts_rx=578060`, `pkts_lost=0`, `jitter_ms=6.00`, `jb_ms=87.13`, `dec_ms=3.84`
  - `BROWSER_AUDIO_IN`: `pkts_rx=91002`, `pkts_lost=0`, `jitter_ms=3.00`, `jb_ms=54.90`, `concealed=0`
- **Clean Disconnect & 15s Reconnect Check**:
  - `CONNECTED video=640x480`
  - `BROWSER_VIDEO`: `rx_fps=29.98`, `dec_fps=30.04`, `disp_fps=30.04`, `rtp_drop=0`, `pkts_rx=4718`, `pkts_lost=0`, `jitter_ms=6.00`, `jb_ms=26.05`, `dec_ms=3.47`
  - `BROWSER_AUDIO_IN`: `pkts_rx=751`, `pkts_lost=0`, `jitter_ms=4.00`, `jb_ms=42.67`, `concealed=375`
