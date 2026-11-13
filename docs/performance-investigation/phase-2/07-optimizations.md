# Phase 2 Optimizations Summary

## 1. Kernel Module Logging Fix (`Opt1`)

- **File**: `aic8800_fdrv` (`aic_btsdio.h`) — patch saved at [`patches/aic8800_fdrv_gate_aicbt_info.patch`](patches/aic8800_fdrv_gate_aicbt_info.patch).
- **Change**: Gated `AICBT_DBG` on `aicwf_dbg_level & LOGDEBUG` and `AICBT_INFO` on `aicwf_dbg_level & LOGINFO`, keeping `AICBT_WARN` and `AICBT_ERR` unconditional.
- **Impact**: Eliminated 533.3/s `aic_btsdio: skb type 3` / `hci0*` prints, reducing `klogd+syslogd` CPU from `8.3%` to `0.0%` and total system CPU from `90.6%` to `82.8%`.

## 2. VENC/VI Overlap Reordering (`Opt3`)

- **File**: [`src/maix_webrtc_maixcam.cpp`](../../../src/maix_webrtc_maixcam.cpp) (`_camera_push_thread`)
- **Change**: Moved `mmf_venc_free()`, `mmf_venc_push2()`, and `mmf_vi_frame_free2()` ahead of `video_frame_push()` so hardware ISP/VENC immediately begins the next frame while `libdatachannel` packetizes and encrypts the current frame.
- **Impact**: Reduced `venc_pop` (`500 us -> 222.1 us`), `venc_push` (`2306 us -> 1347 us`), and recovered browser FPS to `29.04 fps`.

## 3. H.264 RTP Packetizer MTU (`Opt4`)

- **File**: [`src/maix_webrtc_server.hpp`](../../../src/maix_webrtc_server.hpp) (`add_video_track`)
- **Change**: Configured `rtc::H264RtpPacketizer` with `maxFragmentSize = 1360` to prevent IP/UDP fragmentation and fit standard 1500-byte MTU cleanly.

## 4. Opus Complexity 1 & Fixed-Size RX Packet Buffer (`Opt5`)

- **Files**:
  - [`src/maix_webrtc_maixcam.cpp`](../../../src/maix_webrtc_maixcam.cpp): Changed default `MAIX_DIAG_OPUS_COMPLEXITY` from `3` to `1`.
  - [`src/g4c4_opus_rx.hpp`](../../../src/g4c4_opus_rx.hpp): Replaced heap-allocated `std::vector<unsigned char>` in `G4c4OpusRx::Packet` with fixed `std::array<unsigned char, 512>` + `uint16_t size`.
- **Impact**: Reduced `opus_enc` from `1,151 us` (Phase 1) to `807–844 us` and eliminated per-packet heap allocations on the 50 Hz audio RX path.

## 5. `-O3` Compiler Optimization (`Opt6`)

- **File**: [`scripts/build.py`](../../../scripts/build.py)
- **Change**: Upgraded compilation optimization flag from `-O2` to `-O3` for `maix_webrtc_maixcam.cpp` and `libg4c4_sco_write_align.so`.
- **Combined Result**: `81.5%` system CPU (`18.5%` idle headroom), `29.16 fps` browser video, `0` lost packets, `0` concealed audio, `0` ALSA underruns.
