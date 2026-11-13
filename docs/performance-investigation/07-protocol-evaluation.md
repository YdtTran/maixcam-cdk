# 07 — Protocol & Transport Evaluation

## 1. Is WebRTC (`libdatachannel`) the Bottleneck?

**No — not by itself, and replacing it is unnecessary.**

Measured breakdown in full-duplex mode (`PostOpt_FullDuplex`, `SYS CPU = 89.4%`):

- **WebRTC media transport + SRTP + RTP packetization**:
  - Video send (`cam_push` in `libdatachannel` / `libjuice` / SRTP): `~17.4%` CPU at `24.8 fps` (`640x480` H.264), `19.4 ms/frame` inside `webrtc_push`.
  - Audio downlink WebRTC RX (`Test B` minus `Test A`): `+4.3%` system CPU (`~1.8%` in `stream_webrtc-m2`).
  - `RTC worker` (4 threads) + `juice poll` + `SCTP timer` combined: `~3.6%` CPU.
- **Non-WebRTC fixed & kernel costs**:
  - `ISP0_RUN` (hardware camera ISP thread): `10.3%` CPU.
  - `opus_mic` (ALSA SCO read + 8 kHz Opus encode + send): `6.2%` CPU.
  - `opus_rx` (Opus decode 48 kHz -> 8 kHz): `3.9%` CPU.
  - `bt_playback` + `bluealsa`: `0.8% + 2.5% = 3.3%` CPU.
  - `klogd + syslogd` (remaining `aic_btsdio: skb type 3` kernel printk): `6.0%` CPU.
  - Remaining kernel SDIO/MMC/HCI/USB/Wi-Fi IRQ & softirq overhead (`irq 27 mmc1 @ 899.8/s`, `sirq=6.5%`, `kworker`, `aicwf_busrx_thr`/`bustx_thr`).

## 2. Why WebRTC Should Be Kept

1. **Browser-native zero-plugin full-duplex**: Works directly in Chromium/Safari/Firefox over HTTPS/WSS with hardware H.264 pass-through, jitter buffering, ICE/DTLS/SRTP, and browser AEC/NS/AGC on the operator microphone.
2. **Sufficient headroom achieved**: After decoupling `_media_mutex`, switching BT capture to native 8 kHz + `snd_pcm_wait()`, compiling with `-O2`, and silencing `aic8800_fdrv` debug logs, full-duplex operation runs at **89.4% SYS CPU (10.6% idle)** instead of saturating at **97.7%**.
3. **Alternative protocols (RTSP + WebSocket PCM, MJPEG + HTTP, etc.)** would not eliminate `ISP0_RUN` (`10.3%`), `bluealsa` + `bt_playback` (`3.3%`), `aic_btsdio` kernel printk (`6.0%`), SDIO/MMC interrupts, or socket/TLS send overhead, while losing WebRTC jitter buffers, congestion control, and native browser mic/AEC integration.

## 3. Remaining Headroom Levers (If More Margin Is Needed Later)

1. **Kernel `aic_btsdio` log suppression (`-6.0%` CPU)**: Silencing `aic_btsdio: skb type 3` in the vendor kernel module (or lowering console/klogd loglevel if filtered) would immediately reclaim ~6% CPU without touching application code.
2. **Video FPS / Bitrate tuning**: Reducing camera target FPS from `30` to `20` or `15` FPS scales down `cam_push` (`17.4%`) and `ISP0_RUN` (`10.3%`) proportionally if battery or thermal headroom is prioritized over 25–30 fps smoothness.
