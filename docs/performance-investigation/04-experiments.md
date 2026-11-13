# 04 — Reproducible Experiments & Methodology

## 1. Measurement Tools Created

1. **`scripts/perf_probe.py`**:
   - Captures `/proc/uptime`, `/proc/stat`, `/proc/<pid>/stat`, `/proc/<pid>/task/*/stat`, `/proc/<pid>/task/*/status`, `/proc/interrupts`, `/api/audio_stats`, and `/api/perf_stats` over a configurable window (`--duration`).
   - Computes per-process CPU%, per-thread CPU% (user/sys), `vcsw/s`, `nvcsw/s`, `minflt/s`, scheduling policy/priority, IRQ/s, and per-stage microsecond timings.
2. **`scripts/headless_client.mjs`**:
   - Launches `chrome-headless-shell` via CDP (Node 22 built-ins only) against `https://10.172.16.1:8000/` with `--use-fake-device-for-media-stream`, waits for `640x480` WebRTC video, and optionally clicks `#listen-btn` (`--listen`) and `#talk-btn` (`--talk`).
3. **Runtime Diagnostic Flags (`src/g4c4_perf.hpp`)**:
   - `MAIX_DIAG_DISABLE_BT_CAPTURE=1`
   - `MAIX_DIAG_DISABLE_OPUS_RX=1`
   - `MAIX_DIAG_DISABLE_BT_PLAYBACK=1`
   - `MAIX_DIAG_LOCAL_PCM_PLAYBACK=1`
   - `MAIX_DIAG_OPUS_COMPLEXITY=<1|3|5>`
   - `MAIX_DIAG_VIDEO_SLEEP_US=<us>`

## 2. Reproducible Commands

```bash
# Idle (no browser)
python3 scripts/perf_probe.py --duration 5 --label "idle_no_browser"

# Test A: Video only
ssh root@10.172.16.1 '/root/webrtc_service.sh stop; MAIX_DIAG_DISABLE_BT_CAPTURE=1 /root/webrtc_service.sh start'
node scripts/headless_client.mjs --duration 18 &
sleep 5 && python3 scripts/perf_probe.py --duration 6 --label "Test_A_Video_Only"

# Test B: Video + Opus RX (no decode)
ssh root@10.172.16.1 '/root/webrtc_service.sh stop; MAIX_DIAG_DISABLE_BT_CAPTURE=1 MAIX_DIAG_DISABLE_OPUS_RX=1 /root/webrtc_service.sh start'
node scripts/headless_client.mjs --talk --duration 18 &
sleep 5 && python3 scripts/perf_probe.py --duration 6 --label "Test_B_Video_Plus_Opus_RX"

# Test C: Video + Opus RX + Decode (no BT playback)
ssh root@10.172.16.1 '/root/webrtc_service.sh stop; MAIX_DIAG_DISABLE_BT_CAPTURE=1 MAIX_DIAG_DISABLE_BT_PLAYBACK=1 /root/webrtc_service.sh start'
node scripts/headless_client.mjs --talk --duration 18 &
sleep 5 && python3 scripts/perf_probe.py --duration 6 --label "Test_C_Video_Plus_Opus_Decode"

# Test D: Video + Local PCM BT Playback
ssh root@10.172.16.1 '/root/webrtc_service.sh stop; MAIX_DIAG_DISABLE_BT_CAPTURE=1 MAIX_LOCAL_PCM_PLAYBACK=1 /root/webrtc_service.sh start'
# (actual flag: MAIX_DIAG_LOCAL_PCM_PLAYBACK=1)
node scripts/headless_client.mjs --talk --duration 18 &
sleep 5 && python3 scripts/perf_probe.py --duration 6 --label "Test_D_Video_Plus_Local_PCM_BT_Playback"

# Test E: Full Duplex
ssh root@10.172.16.1 '/root/webrtc_service.sh restart'
node scripts/headless_client.mjs --talk --listen --duration 22 &
sleep 5 && python3 scripts/perf_probe.py --duration 8 --label "steady_E_full_duplex"
```

## 3. Key Incremental CPU Deltas (from Test A–E)

| Stage | Incremental System CPU | Incremental `stream_webrtc-m2` CPU |
|---|---:|---:|
| Video only (Test A) | 60.6% | 35.7% |
| + WebRTC Opus RX (Test B - Test A) | +4.3% | +1.8% |
| + Opus 8 kHz Decode (Test C - Test B) | +1.4% | +4.0% (`opus_rx` = 3.0%) |
| + BT SCO Local PCM Playback (Test D - Test A) | **+28.6%** | +4.8% (`bt_playback` = 1.2%; **`klogd+syslogd` = +15.8%**, BT/SDIO kernel = +9%) |
| + BT Mic Uplink + Listen (Baseline Listen - Test A) | **+23.6%** | +7.2–15.5% (`opus_mic` = 13.8%, `klogd+syslogd` = +4.8%) |
