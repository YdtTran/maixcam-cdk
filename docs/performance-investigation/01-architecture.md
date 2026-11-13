# 01 — Execution Architecture & Media Pipeline

## 1. Hardware & OS Platform

Observed on target (`ssh root@10.172.16.1`):

- **Device**: Sipeed MaixCAM Lite (`maixcam-ac10`), CV182x / SG200x SoC.
- **CPU**: **1 core** (`nproc = 1`, `processor : 0`, `hart : 0`, `isa : rv64imafdvcsu`, `mmu : sv39`, C906FDV).
- **Kernel**: `Linux maixcam-ac10 5.10.4-tag- #14 PREEMPT Thu Jan 22 14:08:28 CST 2026 riscv64 GNU/Linux`.
- **Memory**: `128 MB` total RAM (`~43–45 MB` used during active streaming, `0` swap).
- **Wireless / BT Hardware**: AIC8800 SDIO combo chip (`aic8800_bsp`, `aic8800_fdrv`, `aic_btsdio` on `mmc1`, IRQ 27).

## 2. Operating System Processes

| Process | Role | Notes |
|---|---|---|
| `stream_webrtc-m2` | Main C++ native media, WebRTC, HTTP/WSS server | Launched by `/root/webrtc_service.sh` |
| `webrtc_autostar` (`supervisor.sh`) | Watchdog daemon checking AP + `stream_webrtc-m2` + ports `8000/8001` every 10s | `target_services/standalone/supervisor.sh` |
| `webrtc_service.sh bt-watch` | Polls `bluealsa-aplay -L` every 5s and reconnects HFP/SCO if missing | `target_services/webrtc_service.sh` |
| `bluealsa` | BlueALSA v4.1.1 daemon (`-p a2dp-source -p hfp-ag -S`) with `LD_PRELOAD=libg4c4_sco_write_align.so` | Bridges BlueZ SCO/A2DP to ALSA PCM |
| `bluetoothd` | BlueZ 5.68 daemon | Manages HCI/D-Bus pairing & profiles |
| `hostapd` | Standalone 5 GHz Wi-Fi AP (`wlan0`, channel 36, `192.168.4.1/24`, open) | `target_services/standalone/network.sh` |
| `dnsmasq` | DHCP/DNS for `wlan0` AP clients | `192.168.4.x` |
| `klogd` / `syslogd` | Kernel & system log daemons writing to `/var/log/messages` | Consumes 5–15.8% CPU during BT SCO due to `aic_btsdio` / `aicwf` log spam |
| `dbus-daemon` | System D-Bus | Used by `bluetoothd` / `bluealsa` |
| `cvitask_*` / `vi_event_handle` / `aicwf_*` | Kernel threads for ISP, VPSS, VENC, VI, and AIC8800 Wi-Fi/BT SDIO | Kernel-space workers |

## 3. `stream_webrtc-m2` Thread Map

All threads inside `stream_webrtc-m2` (with `pthread_setname_np()` instrumentation added in `src/g4c4_perf.hpp`):

| Thread Name | Source / Owner | Scheduling (`pol` / `prio`) | Responsibility & Lifecycle | Blocking Behavior |
|---|---|---|---|---|
| `stream_webrtc-m` | `main.cpp` (`_main`) | `SCHED_OTHER` (0) / 20 | Initializes `Camera(640,480,YVU420SP,30fps)`, `WebRTC`, loops `time::sleep_ms(10)` until exit | Sleeps 10 ms |
| `ISP0_RUN` | MaixCDK / Sophgo ISP middleware | **`SCHED_RR` (2) / -81 (RT)** | Real-time ISP 3A / sensor frame processing | Blocks on VI/ISP hardware (~60 vcsw/s) |
| `cam_push` | `src/maix_webrtc_maixcam.cpp` (`_camera_push_thread`) | `SCHED_OTHER` (0) / 20 | Pops VI frame (`mmf_vi_frame_pop2`), pops H.264 VENC stream (`mmf_venc_pop`), pushes to `MaixWebRTCServer::video_frame_push()`, feeds VI frame to VENC (`mmf_venc_push2`) | `mmf_vi_frame_pop2()` blocks ~20–22 ms waiting for next 30 fps frame when track open; sleeps 10 ms when track closed |
| `opus_mic` | `src/maix_webrtc_maixcam.cpp` (`_opus_mic_thread`) | `SCHED_OTHER` (0) / 20 | Reads BT SCO mic via `G4c4BtCapture::read()`, accumulates in `MaixAudioUplinkDSP`, encodes Opus (`opus_encode`), pushes via `audio_frame_push()` | Baseline: non-blocking `snd_pcm_readi` + 5 ms sleep on `-EAGAIN`; Optimized: `snd_pcm_wait(pcm_, 25)` |
| `opus_rx` | `src/g4c4_opus_rx.hpp` (`G4c4OpusRx::run`) | `SCHED_OTHER` (0) / 20 | Created per `PeerConnection` in `add_audio_track()`. Pops Opus RTP frames from `queue_`, decodes via `opus_decode(8000, 1)`, enqueues PCM to `G4c4BtPlayback` | Waits on `cv_.wait_for(..., 250ms)` |
| `bt_playback` | `src/g4c4_bt_playback.hpp` (`G4c4BtPlayback::worker_loop`) | `SCHED_OTHER` (0) / 20 | Owned by `MaixWebRTCServer::_shared_playback`. Opens `bluealsa:...,PROFILE=sco` (8 kHz mono S16_LE), writes PCM via blocking `snd_pcm_writei()` | Blocks ~19.8 ms per 20 ms write when Talk ON; idle `cv_.wait` when Talk OFF (optimized) |
| `pcm-io` | ALSA / BlueALSA client thread | `SCHED_OTHER` (0) / 20 | Internal ALSA/BlueALSA I/O thread spawned during `G4c4BtCapture` | ~130 vcsw/s when capture open |
| `RTC worker` (×4) | `libdatachannel` thread pool | `SCHED_OTHER` (0) / 20 | Handles incoming/outgoing DTLS, SRTP, SCTP, RTCP, `onFrame` callbacks | Event-driven |
| `RTC server` / `RTC poll` | `libdatachannel` WebSocketServer | `SCHED_OTHER` (0) / 20 | Built-in WSS signaling server (`0.0.0.0:8001`) | `poll()` |
| `juice poll` | `libjuice` ICE agent | `SCHED_OTHER` (0) / 20 | ICE connectivity checks and UDP socket I/O | `poll()` |
| `SCTP timer` | `usrsctp` | `SCHED_OTHER` (0) / 20 | DataChannel SCTP timer (~90 vcsw/s) | Periodic 10 ms timer |
| `http_srv` (+workers) | `src/maix_webrtc_http_server.hpp` (`cpp-httplib`) | `SCHED_OTHER` (0) / 20 | Serves HTTPS (`0.0.0.0:8000`) cockpit HTML/JS and `/api/*` endpoints | `select()`/`poll()` |
| `sig_wdog` | `src/maix_webrtc_signaling_server.hpp` | `SCHED_OTHER` (0) / 20 | Checks signaling client activity every 5s | `cv_.wait_for(..., 5s)` |

## 4. Media & Control Pipelines

1. **Video Pipeline**:
   `Camera Sensor (640x480@30fps YVU420SP)` → `ISP0_RUN` / `VPSS` → `mmf_vi_frame_pop2()` → `mmf_venc_push2()` → `Hardware H.264 VENC (IRQ 37)` → `mmf_venc_pop()` → `cam_push` → `MaixWebRTCServer::video_frame_push()` → `libdatachannel H264RtpPacketizer + SRTP` → `UDP`.
2. **Audio Uplink Pipeline (BT Mic → Browser)**:
   `EDIFIER X2 Plus Mic` → `Bluetooth eSCO (CVSD 8 kHz mono)` → `aic_btsdio (IRQ 27)` → `bluealsa` → `G4c4BtCapture::read()` → `MaixAudioUplinkDSP` → `opus_encode()` → `MaixWebRTCServer::audio_frame_push()` → `libdatachannel OpusRtpPacketizer + SRTP` → `UDP`.
3. **Audio Downlink Pipeline (Browser → BT Earbuds)**:
   `Browser Mic` → `UDP/SRTP` → `libdatachannel OpusRtpDepacketizer` → `onFrame()` → `G4c4OpusRx::enqueue()` → `opus_rx` (`opus_decode` 8 kHz mono; `G4c4Resampler` bypassed for native SCO) → `G4c4BtPlayback::enqueue_pcm()` → `bt_playback` (`snd_pcm_writei` 8 kHz mono) → `bluealsa` (`libg4c4_sco_write_align.so`) → `aic_btsdio` → `eSCO` → `EDIFIER X2 Plus`.
