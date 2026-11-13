# Investigation Validation & Acceptance Matrix

## 1. Phase 2 Goal Verification

| Goal | Target | Verified Result | Status |
|---|---|---|---|
| 1. Full-duplex CPU reduction | <= 80% | **73.40% avg** (1800s light poll) / **81.5%** (8s heavy `perf_probe` SSH poll) | Achieved under normal operation |
| 2. Video FPS recovery | ~30 fps | **29.57 fps** (30-min browser `disp_fps`, `0` lost pkts) | Achieved |
| 3. Eliminate BT kernel logging | 0 spam/s | **0/s** (`aic_btsdio` static 95 -> 95 over 30 min; `klogd+syslogd` 8.3% -> 0.0%) | Achieved |
| 4. Simultaneous video + BT mic + BT downlink | Reliable full-duplex | Verified across USB, 5 GHz STA, 2.4 GHz STA, and 30-min run (`0` underruns, `0` lost pkts) | Achieved |
| 5. 30-minute stability test | 30 min continuous | **1820s continuous**, 0 crashes, 0 leaks (`VmRSS` 23.82 -> 24.05 MB), clean reconnect | Achieved |
| 6. Latency & jitter evaluation | Measured bounds | Video jitter 4–6 ms, Audio jitter 3–4 ms; internal stages quantified in `06-latency.md` | Achieved |
| 7. Standalone Wi-Fi AP & HTTPS cockpit | Operational | `MaixCAM` 5 GHz AP (ch 36, `192.168.4.1/24`) restored; `/` and `/api/perf_stats` HTTP 200 | Achieved |

## 2. Preserved Features & Rollback Readiness

- All Phase 1 optimizations preserved (`-O2` upgraded to `-O3`, Opus complexity lowered `3 -> 1`).
- Original vendor `aic8800_fdrv.ko` backed up at `/mnt/system/ko/3rd/aic8800_fdrv.ko.orig` on device.
- `deploy.py` rollback script preserved on device for `stream_webrtc-m2`.
- HTTPS web cockpit (`INDEX_HTTP=200`, `PERF_HTTP=200`) and standalone 5 GHz AP verified active after all tests.
