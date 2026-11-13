# 03 — Ranked Bottlenecks & Root Causes

## Rank 1: `_media_mutex` Serialization Across Video & Audio `track->send()`
- **Source**: `src/maix_webrtc_server.hpp` (`video_frame_push()`, `audio_frame_push()`, `is_video_track_open()`, `is_audio_track_open()`).
- **Evidence**:
  - `video_frame_push()` held `_media_mutex` during byte-by-byte `rtc::binary::push_back()` and `_video_track->send()` (**9.67 ms/frame** in Test A; **33.16 ms/frame** under contention in Test E).
  - `audio_frame_push()` held `_media_mutex` during `_audio_track->send()` (**1.49–8.20 ms/packet**).
  - In `steady_E_full_duplex`, `_media_mutex` was held **13,893 ms out of 18,870 ms (73.6% of wall time)** and threads spent **9,451 ms (50.1% of wall time)** blocked waiting on `_media_mutex`, dropping video from 29.5 fps to 26.5 fps (and 18.0 fps during connect transients).

## Rank 2: Kernel Log Spam (`klogd` + `syslogd`) During Bluetooth SCO (10.4%–15.8% CPU)
- **Source**: Kernel modules `aic8800_fdrv` (`aicwf_dbg_level = 1039`) and `aic_btsdio`.
- **Evidence**:
  - Test A (no BT SCO): `klogd` = 0.0%, `syslogd` = 0.0%, `IRQ 27 (mmc1)` = 0.4/s.
  - Test D (local PCM BT SCO playback): `klogd` = **8.1%**, `syslogd` = **7.7%** (**15.8% CPU**), `IRQ 27` = 1365.1/s.
  - Test E (full duplex): `klogd` = **5.5%**, `syslogd` = **4.9%** (**10.4% CPU**).
  - `/var/log/messages` receives `AICWFDBG(LOGDEBUG) rwnx_rx_handle_msg msg->id:0xc12` and `aic_btsdio: skb type 3` ~133 times/sec per direction.
  - Confirmed `echo 0 > /sys/module/aic8800_fdrv/parameters/aicwf_dbg_level` immediately stops all `AICWFDBG(LOGDEBUG)` messages.

## Rank 3: `cam_push` WebRTC Packetization/SRTP + Per-Frame `malloc`/`free` (21–24% CPU)
- **Source**: `src/maix_webrtc_maixcam.cpp` (`_camera_push_thread()`), `src/maix_webrtc_server.hpp`.
- **Evidence**:
  - `mmf_vi_frame_pop2()` **blocks** ~21–22 ms waiting for the next 30 fps frame (`vi_empty = 0.0/s`, `venc_empty = 0.0/s`) — **no busy polling** in `cam_push`.
  - Per-frame `malloc()`/`free()` + `rtc::binary` allocation caused `minflt = 348–449/s`.
  - `webrtc_push` (`_video_track->send()`) takes **9.67–10.50 ms/frame** (H.264 NALU parsing, RTP packetization, SRTP encryption, UDP send) — ~29% of 1 core at 30 fps (`23.5%` `/proc` CPU).

## Rank 4: `opus_mic` 48 kHz ALSA Resampling + 48 kHz `opus_encode` + 5 ms Polling (13.8%–15.6% CPU)
- **Source**: `src/g4c4_bt_capture.hpp` (`G4c4BtCapture::open()`, `read()`), `src/maix_webrtc_maixcam.cpp` (`_opus_mic_thread()`).
- **Evidence**:
  - BlueALSA SCO hardware rate is **8000 Hz mono** (`bluealsa-aplay -L`), but `G4c4BtCapture` requested **48000 Hz**, forcing ALSA `rate` plugin software resampling (6× samples) and `opus_encode(48000, 1)` on 960 samples/frame (**4.14 ms/frame** at complexity 5; **10.46 ms** wall time under contention).
  - `SND_PCM_NONBLOCK` + `5 ms` `sleep_for` on `-EAGAIN` caused `empty_reads = 92–249/s` (`vcsw = 115–177/s`).

## Rank 5: `ISP0_RUN` Real-Time Sensor/ISP Processing (10.3%–10.9% CPU)
- **Source**: MaixCDK / Sophgo middleware (`SCHED_RR`, prio `-81`).
- **Evidence**: Consumes ~10.5% CPU continuously whenever `Camera` is open (even with no browser connected). Necessary for 640×480@30fps ISP pipeline.

## Rank 6: Downlink Audio (`opus_rx` 3.0%–4.0% CPU + `bt_playback` 0.6%–1.2% CPU + `bluealsa` 1.3%–2.8% CPU)
- **Source**: `src/g4c4_opus_rx.hpp`, `src/g4c4_bt_playback.hpp`.
- **Evidence**:
  - `uses_native_sco()` is `true`; `G4c4Resampler` is **confirmed bypassed** (`process_calls = 0`).
  - `opus_decode(8000, 1)` takes **0.73–0.88 ms/frame** (**3.0%–4.0% CPU** at 50 pkt/s).
  - `bt_playback` `snd_pcm_writei()` blocks ~19.8 ms/write with **0 `-EAGAIN`**, **0 partial writes**, **0 underruns**, **1.0%–1.2% CPU**.
  - **Ruled out**: Neither `opus_rx` nor `bt_playback` ALSA writes are CPU bottlenecks.

## Rank 7: Compiler Flag `-Os` (`build/build-metadata.json`) & Idle Wakeups
- `maix_webrtc_maixcam.cpp` (and all inline headers) compiled with `-Os` instead of `-O2`.
- `bt_playback` woke up every 20 ms (`49 vcsw/s`) even when `talk_active_ == false`.
