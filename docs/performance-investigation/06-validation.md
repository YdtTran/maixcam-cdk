# 06 — Post-Optimization Validation

## 1. Test Environment

- **Device**: Sipeed MaixCAM Lite (`nproc=1`, `rv64imafdvcsu`, 128 MB RAM, Linux `5.10.4-tag-`).
- **Binary**: `stream_webrtc-m2` (`SHA256: f08fe00aa5c15275a3c3739bd565d459e4176fa6c063b02c9b4909029cc73f34`), deployed via `scripts/standalone/deploy.py --target root@10.172.16.1 --skip-build` (`PID 1183`).
- **Bluetooth**: `EDIFIER X2 Plus` (`6C:16:29:7B:00:D4`), `SCO (CVSD): S16_LE 1 channel 8000 Hz` active for both capture and playback (`bluealsa v4.1.1`).
- **Wi-Fi**: Standalone AP (`wlan0`, `ssid MaixCAM`, 5 GHz channel 36, `STATE=READY`), `aicwf_dbg_level=0`.
- **Client**: `scripts/headless_client.mjs --talk --listen --duration 22` driving Chromium headless shell over HTTPS/WSS (`640x480` video + bidirectional Opus audio).
- **Measurement Windows**: `17.04 s` (`PostOpt_FullDuplex`), `21.21 s` (`PostOpt_FullDuplex_Clean`, `p_dt=29.26 s`), and `9.95 s` (`PostOpt_Idle`) via `scripts/perf_probe.py`.

## 2. Before vs After Comparison (Full Duplex: Video + Listen + Talk)

| Metric | Baseline (`Test E`) | Post-Opt (`PostOpt_FullDuplex_Clean`) | Delta |
|---|---:|---:|---:|
| **Total System CPU** | **97.7%** (idle `2.3%`) | **87.5%** (idle `12.5%`) | **-10.2%** (+10.2% headroom) |
| User / Kernel / SoftIRQ | `56.1% / 33.2% / 8.4%` | `41.3% / 39.8% / 6.4%` | `-14.8%` usr, `-2.0%` sirq |
| `stream_webrtc-m2` total | ~50.1% | **48.1%** (17s run: `38.9%`) | **-2.0 to -11.2%** |
| `cam_push` thread | ~22–24% | **21.0%** (`24.6 fps`, `copy=39.6 us`) | **-1 to -3%** |
| `ISP0_RUN` thread | 10.5% | **10.7%** | ~0% (hardware ISP loop) |
| `opus_mic` thread | **13.8–15.6%** | **7.8%** (17s run: `6.2%`) | **-6.0 to -9.4%** |
| `opus_enc` avg / frame | **10,460 us** | **1,151.3 us** | **9.1x faster** |
| `opus_mic` empty reads | **115–249 /s** | **1.3 /s** (`snd_pcm_wait` blocking) | **-99%** |
| `opus_rx` thread | 3.0–4.0% | **3.5%** (`45.3 pkt/s`, `803.5 us/dec`) | ~0% |
| `bt_playback` thread | 1.0–1.2% | **1.0%** (`0` underruns, `0` EAGAIN) | ~0% |
| `_media_mutex` wait time | **9,451 ms** (`50.1%` window) | **23.15 ms** (`0.08%` of `29.26 s`) | **-99.8%** |
| `_media_mutex` hold time | **13,893 ms** | **155.91 ms** (`23.8 us/lock`) | **-98.9%** |
| `klogd + syslogd` | **10.4–15.8%** | **8.3%** (17s run: `6.0%`) | **-2.1 to -9.8%** |
| `bluealsa` | 2.8% | **3.1%** | ~0% |

## 3. Idle & Reconnect Verification (`PostOpt_Idle`)

- After client disconnect (`PostOpt_Idle`, `9.95 s` window): `SYS CPU=53.4%` (`46.6%` idle), `stream_webrtc-m2=11.6%` (down from `14.7%` baseline idle), `opus_mic=0.9%` (down from `2.0%`), `bt_playback` **0%** (`cv_.wait` idle sleep).
- A second consecutive `headless_client.mjs --talk --listen --duration 8` session connected cleanly (`CONNECTED video=640x480`, `LISTEN_CLICKED`, `TALK_CLICKED`).

## 3. Functional & Reliability Verification

- **Video**: Headless Chromium received `640x480` H.264 video (`CONNECTED video=640x480`), `24.8 fps` actual encoded/sent rate, `0` empty `vi_pop`/`venc_pop` polls.
- **Audio Uplink**: `55.2 pkt/s` sent, `0.0/s` send failures, `44.2/s` Opus frames encoded cleanly at `8000 Hz`.
- **Audio Downlink**: `42.7 pkt/s` decoded, `0` decode errors, `0` ALSA underruns, `0` `EAGAIN`/`partial`/`zero` writes in `bt_playback`.
- **RAM**: `41 MB / 128 MB` used (`80 MB` available, `0` swap).
- **Wi-Fi / BT Coexistence Note**: Avoid running `hciconfig hci0 down/up` at runtime; on the shared AIC8800 SDIO combo chip it causes `wlan0` AP beacon failure (`AP_FAILED`), which requires a reboot to recover. Normal boot and `deploy.py` operation leave both `wlan0` AP and `EDIFIER X2 Plus` SCO healthy.
