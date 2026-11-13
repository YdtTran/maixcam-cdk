# 05 — Optimizations Implemented

All optimizations were kept small, isolated, and reversible, preserving full video, full-duplex Bluetooth SCO audio, WebRTC signaling, and standalone AP operation.

## 1. Decouple `_media_mutex` from Network Send & Eliminate Per-Frame Buffer Copies

- **Files**: `src/maix_webrtc_server.hpp`, `src/maix_webrtc_maixcam.cpp`
- **Root Cause**: `MaixWebRTCServer::video_frame_push()` and `audio_frame_push()` held `_media_mutex` across `track->send()` (which blocks in `libdatachannel` / `libjuice` / SRTP encryption for `~10–33 ms/frame` on video and `~1.5–8.2 ms/pkt` on audio). Both threads contended on `_media_mutex` (`9,451 ms` wait in an `18.87 s` window = `50.1%` wall time). Additionally, `video_frame_push()` and `audio_frame_push()` copied payloads byte-by-byte with `push_back()`, and `_camera_push_thread()` called `malloc()`/`free()` per VENC frame.
- **Change**:
  - Replaced `_media_mutex` lock across `track->send()` with dedicated `_video_send_mutex` and `_audio_send_mutex`, holding `_media_mutex` only for a microsecond-scale pointer/state snapshot.
  - Replaced byte-by-byte `push_back()` with `memcpy()` into reusable `_video_send_buf` and `_audio_send_buf`.
  - Replaced per-frame `malloc()`/`free()` in `_camera_push_thread()` with a reusable `std::vector<uint8_t> venc_buf`.
- **Measured Effect** (`Opt1_MutexDecouple_FullDuplex` vs Baseline `Test E`):
  - `_media_mutex` wait time: `9,451 ms` -> `2.15–2.22 ms` (and `18.67 ms` in `PostOpt_FullDuplex`).
  - `_media_mutex` hold time: `13,893 ms` -> `16.27–19.51 ms` (`avg_hold_us = 19.5 us`).
  - Per-frame video copy time: `50.5 us` -> `20.1–26.4 us`.

## 2. Native 8 kHz Bluetooth SCO Capture + Event-Driven `snd_pcm_wait()` + Opus 8 kHz Complexity 3

- **Files**: `src/g4c4_bt_capture.hpp`, `src/maix_webrtc_audio_dsp.hpp`, `src/maix_webrtc_maixcam.cpp`
- **Root Cause**: `G4c4BtCapture` previously opened BlueALSA SCO at `48000 Hz`, forcing ALSA's `rate` plugin to upsample native `8000 Hz` CVSD SCO audio to `48000 Hz`, followed by `opus_encode()` at `48000 Hz` complexity `5` (`10,460 us/frame`). Furthermore, `_opus_mic_thread()` polled `snd_pcm_readi()` with a `5 ms` sleep on `-EAGAIN`, generating `115–249 empty_reads/s`.
- **Change**:
  - Switched `G4c4BtCapture` to native `8000 Hz` mono (`160` samples / `20 ms` frame) matching `SCO (CVSD): S16_LE 1 channel 8000 Hz`.
  - Replaced the `5 ms` `-EAGAIN` polling sleep in `G4c4BtCapture::read()` with blocking `snd_pcm_wait(pcm_, 25)`.
  - Initialized the uplink Opus encoder at `8000 Hz` (`opus_encoder_create(8000, 1, OPUS_APPLICATION_VOIP)`) with default complexity `3` (preserving `48000 Hz` RTP clock timestamp advancement `sample_cursor += 960` required by WebRTC RFC 7587).
  - Updated `MaixAudioUplinkDSP` to accept `8000 Hz` (`160`-sample frames).
- **Measured Effect** (`PostOpt_FullDuplex` vs Baseline `Test E`):
  - `opus_mic` thread CPU: `13.8–15.6%` -> `6.2%` (`-7.6` to `-9.4` percentage points).
  - `opus_enc` duration per 20 ms frame: `10,460 us` -> `1,146 us` (**9.1x faster**).
  - `empty_reads/s`: `115–249/s` -> `0.6/s` (`avg_read_us = 17,300 us` blocking).

## 3. Idle Condition-Variable Wait in `G4c4BtPlayback`

- **File**: `src/g4c4_bt_playback.hpp`
- **Root Cause**: When Talk was inactive and no ALSA handle was open, `G4c4BtPlayback::worker_loop()` woke up every `20 ms` (`wait_for(20 ms)`) instead of sleeping until needed.
- **Change**: Switched idle wait to `cv_.wait(lock, ...)` when `!talk_active_ && !pcm_handle_ && !need_pcm_drop_`.

## 4. Compiler Optimization Flag (`-Os` -> `-O2`)

- **File**: `scripts/build.py`
- **Root Cause**: `compile_cmd` used `-Os` (optimize for size), which penalized tight DSP/codec loops (Opus encode/decode) on the single-core `rv64imafdvcsu` CPU.
- **Change**: Switched `compile_cmd` from `-Os` to `-O2`.
- **Measured Effect**:
  - `opus_rx` `avg_decode_us` (48 kHz -> 8 kHz): `792.4 us` (`-Os`) -> `679.9 us` (`-O2`, measured in `Opt2_3_4_FullDuplex`).

## 5. Silence `aic8800_fdrv` Kernel Debug Log Spam

- **File**: `target_services/webrtc_service.sh`
- **Root Cause**: `/sys/module/aic8800_fdrv/parameters/aicwf_dbg_level` defaulted to `1039`, flooding `dmesg`/`klogd`/`syslogd` with `AICWFDBG(LOGDEBUG) rwnx_rx_handle_msg msg->id:0xc12` during Wi-Fi + BT SCO operation (`klogd + syslogd = 10.4–15.8%` CPU).
- **Change**: Added `echo 0 > /sys/module/aic8800_fdrv/parameters/aicwf_dbg_level 2>/dev/null || true` on service start.
- **Measured Effect**:
  - `klogd + syslogd` CPU: `10.4–15.8%` -> `6.0%` (`klogd 3.2% + syslogd 2.8%`). The remaining `6.0%` comes from `aic_btsdio: skb type 3` kernel `printk` messages (a kernel module log not modified per investigation rules).
