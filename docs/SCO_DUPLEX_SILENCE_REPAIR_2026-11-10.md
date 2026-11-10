# MaixCAM Bluetooth SCO/HFP Playback Silence: Root-Cause Isolation & Repair

**Date:** 2026-11-10  
**Repository:** `maixcam-cdk`  
**Reference Implementation:** [`YdtTran/maixcam-mic`](https://github.com/YdtTran/maixcam-mic) (`2026-10-11` verified full-duplex SCO baseline)  
**Target Hardware:** MaixCAM (`maixcam-ac10`, RISC-V musl Linux `5.10.4-tag-`), AIC8800 / RivieraWaves SDIO Bluetooth controller (`hci0`), `EDIFIER X2 Plus` earbuds (`6C:16:29:7B:00:D4`), BlueZ `5.68`, BlueALSA `4.1.1` (`HFP/SCO`, `CVSD`, `8000 Hz` mono `S16_LE`).

---

## 1. Root Cause(s) and Confidence Levels

| Rank | Root Cause / Hypothesis | Status | Confidence | Summary of Mechanism |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **Host SCO MTU `255:4` vs. `120:4` Packet Framing Mismatch** | **CONFIRMED** | **High (100%)** | At power-on default `SCO MTU: 255:4`, BlueALSA's `sco_cvsd_enc_thread` reads `SCO_OPTIONS` (`mtu = 255`, aligned to 254 bytes by `libg4c4_sco_write_align.so`) and writes 254-byte SCO payloads every ~15.875 ms (`735` TX packets over 12s). The AIC8800 SDIO controller / negotiated eSCO link (`interval=12` slots = 7.5 ms, `air_pkt_len=60` = 120 host PCM bytes) produces **complete physical silence** when fed 254-byte host SCO frames, even though ALSA and HCI TX counters increment with 0 errors. Setting `hciconfig hci0 scomtu 120:4` while `hci0` is `UP` and **before** the SCO socket opens causes BlueALSA to send 120-byte SCO payloads every 7.5 ms (`1,572` TX packets over 12s), which is **immediately and clearly audible**. |
| **2** | **AIC8800 SDIO Firmware Command Queue Wedge (`RWNX_CMD_MGR_STATE_CRASHED`)** | **CONFIRMED** | **High (100%)** | In `aic8800_fdrv` (`rwnx_cmds.c` / `btsdio.c`), every HCI command, ACL frame, and SCO packet is wrapped in `rwnx_sdio_bt_send_req()` (`TDLS_SDIO_BT_SEND_REQ`, `reqcfm = 1`). When concurrent HCI/D-Bus churn causes a command timeout (`command 0x0c14 tx timeout`), `cmd_mgr->state` enters `RWNX_CMD_MGR_STATE_CRASHED` permanently until reboot. Subsequent `cmd_mgr_queue()` calls return `-EPIPE` (`AICWFDBG(LOGERROR) cmd queue crashed` / `bt send no cfm`), which `btsdio_work` ignores—so ALSA writes succeed silently while 0 packets reach the radio and SCO RX freezes. |
| **3** | **Duplicate Connection Managers & Unconditional 5s `ConnectProfile` Churn** | **CONFIRMED** | **High (100%)** | `webrtc_service.sh bt-watch` unconditionally executed `org.bluez.Device1.ConnectProfile(HFP)` every 5 seconds even when SCO was already active, while `/etc/init.d/S42bt_manager` (`bt_event_manager.sh`) ran duplicate background loops and treated `< ACL ... state 5` (paging) in `hcitool con` as connected. |
| **4** | **Pre-Warmed Playback Handle Orphaning BlueALSA SCO Socket Across Disconnects** | **CONFIRMED** | **High (100%)** | `G4c4BtPlayback::worker_loop` pre-warmed its ALSA playback handle at startup and kept `pcm_handle_` open on Talk OFF. When the headset disconnected while `G4c4BtPlayback` held the playback PCM open, BlueALSA retained an orphaned kernel SCO socket, causing subsequent `ConnectProfile(HFP)` calls after ACL reconnect to fail with `br-connection-refused` / `NoReply` until the playback handle was closed. |
| **5** | **Aggressive Capture 250 ms Stall Timeout & 500 ms Retry Storm** | **CONFIRMED (Secondary)** | **High (95%)** | `_opus_mic_thread` reset and reopened BlueALSA SCO capture every 750 ms (`250 ms` stall + `500 ms` sleep) during SCO setup or disconnect (`1,516` resets when wedged, `8` resets in 4s on disconnect), adding D-Bus/PCM churn. |
| **6** | **Playback ALSA Timing (20 ms Period / 80 ms Buffer) & Active Speech Gap Pacing** | **CONFIRMED (Stability)** | **Medium-High (85%)** | Explicit `period_time = 20000` us (160 samples) and `buffer_time = 80000` us (640 samples) at 8000 Hz mono with 20 ms silence pacing during active Talk packet gaps (<= 500 ms) matches the `maixcam-mic` baseline (`talkback_receiver.py`) and keeps period write latency locked at ~19.9 ms with 0 underruns. |

---

## 2. Evidence from Controlled Isolation Experiments

### Experiment A: Controller Health Audit & Recovery
- **Initial State**: `hci0` was wedged in `RWNX_CMD_MGR_STATE_CRASHED`. `dmesg` contained `1,440` occurrences of `AICWFDBG(LOGERROR) cmd queue crashed` and `bt send no cfm` following `rwnx_cmd_mgr_llind: command 0x0c14 tx timeout`. `hciconfig -a hci0` counters were frozen (`RX sco: 26398`, `TX sco: 1`) and `/tmp/webrtc.log` showed `attempt=1516` capture resets.
- **Recovery**: Controlled reboot via `scripts/standalone/test_reboot.py` (`boot_id` `93a15587-0445-4af7-a017-a083acbfb54f`) cleared `cmd_mgr->state` to `0` kernel errors.

### Experiment B: Eliminate Connection Churn at `SCO MTU: 255:4`
- **Setup**: Stopped `/etc/init.d/S42bt_manager` and `webrtc_service.sh bt-watch`, connected `6C:16:29:7B:00:D4` once cleanly, and preserved the power-on default `SCO MTU: 255:4`.
- **Execution**: Ran a 12-second WebRTC full-duplex test (`scripts/standalone/test_device.py --cycles 1 --duration 12`):
  - `dmesg` crash count: `0`.
  - `RX sco`: `+8,517` packets (`557` uplink WebRTC audio frames, `336` video frames at `29.67 fps`).
  - `TX sco`: `+735` packets (`189,630` TX bytes / `735` = `258` bytes/packet = `254-byte` SCO payload every `15.875 ms`).
  - `playback.frames_written`: `93,760` (`alsa_errors: 0`, `underruns: 0`).
- **Physical Result (User Confirmed)**: **SILENT**.

### Experiment C: `SCO MTU: 120:4` Applied While `hci0` UP Before SCO Socket Open
- **Setup**: Closed SCO clients, disconnected `6C:16:29:7B:00:D4`, applied `hciconfig hci0 scomtu 120:4` while `hci0` was `UP`, reconnected `6C:16:29:7B:00:D4` and HFP profile, and restarted `stream_webrtc-m2`.
- **Execution**: Ran the identical 12-second WebRTC full-duplex test (`scripts/standalone/test_device.py --cycles 1 --duration 12`):
  - `hciconfig -a hci0` verified `SCO MTU: 120:4`.
  - `dmesg` crash count: `0`.
  - `RX sco`: `+5,470` packets (`557` uplink WebRTC audio frames, `335` video frames at `28.37 fps`).
  - `TX sco`: `+1,572` packets (`194,928` TX bytes / `1,572` = `124` bytes/packet = `120-byte` SCO payload every `7.5 ms`).
  - `playback.frames_written`: `94,560` (`alsa_errors: 0`, `underruns: 0`).
- **Physical Result (User Confirmed)**: **AUDIBLE ("yes can here clearly")**.

### Experiment D & E: Capture Lifecycle & Playback Socket Ownership Across Disconnects
- **Observation**: Disconnecting `6C:16:29:7B:00:D4` while `G4c4BtPlayback` held its pre-warmed playback PCM open left an orphaned kernel SCO socket in `/proc/net/sco`, causing `ConnectProfile(HFP)` to fail with `br-connection-refused` after ACL reconnect until `stream_webrtc-m2` closed the playback handle.
- **Resolution**: Opening the SCO playback PCM on demand when Talk audio arrives and calling `close_device()` on Talk OFF or `> 500 ms` stream idle eliminated the orphaned SCO socket and enabled automatic disconnect recovery in `<= 7s`.

---

## 3. Exact Files Changed and Why

1. `target_services/webrtc_service.sh`:
   - Stops `/etc/init.d/S42bt_manager` in `start()` so `webrtc_service.sh` is the single authoritative Bluetooth connection manager.
   - Adds `prepare_bluetooth_controller()` to configure `hciconfig hci0 up`, `hciconfig hci0 voice 0x0060`, and `hciconfig hci0 scomtu 120:4` while `hci0` is `UP`, disconnecting any early-boot `255:4` `eSCO` link once so the new socket negotiates `120:4`.
   - Makes `connect_bluetooth_mic()` idempotent: returns immediately when `bluealsa-aplay -L` already lists `DEV=6C:16:29:7B:00:D4,PROFILE=sco`, avoiding periodic HCI/D-Bus commands during active streaming.
2. `src/maix_webrtc_maixcam.cpp`:
   - Increases capture stall timeout in `_opus_mic_thread` from `250 ms` to `2000 ms` and recovery sleep from `500 ms` to `1500 ms` (with 5 ms empty-read poll sleep), eliminating the 750 ms PCM reset storm.
3. `src/g4c4_bt_capture.hpp`:
   - Configures explicit ALSA `hw_params` in `G4c4BtCapture::open` for `period_time = 20000` us (20 ms) and `buffer_time = 80000` us (80 ms), with fallback to `snd_pcm_set_params(..., 80000)`.
4. `src/g4c4_bt_playback.hpp`:
   - Configures explicit ALSA `hw_params` in `G4c4BtPlayback::open_device` for `native_sco_` (`period_time = 20000` us / 160 samples, `buffer_time = 80000` us / 640 samples at 8000 Hz mono).
   - Opens the playback PCM on demand when Talk audio arrives, injects 20 ms silence frames (`160` samples) during active SCO Talk packet gaps `<= 500 ms`, and calls `close_device()` on Talk OFF or `> 500 ms` stream idle (`notify_stream_idle()`).
5. `scripts/standalone/test_bluetooth_recovery.sh` & `scripts/tests/test_sco_duplex_repair.py`:
   - Added unit/regression tests for startup ordering, `255:4` eSCO link reset, idempotent `connect_bluetooth_mic`, and capture/playback contracts, and updated the standalone Bluetooth recovery script to assert both `PROFILE=sco` restoration and `SCO MTU: 120:4`.

---

## 4. Tests Performed and Outcomes

| Test | Command | Outcome | Key Metrics |
| :--- | :--- | :--- | :--- |
| **Unit / Regression Suite** | `python3 -m unittest -v scripts/tests/test_sco_duplex_repair.py` | **PASS** (4/4 tests) | Verified `scomtu 120:4` ordering, `255:4` eSCO reset, idempotent `connect_bluetooth_mic`, and capture/playback parameters. |
| **2-Cycle Full-Duplex WebRTC** | `python3 scripts/standalone/test_device.py --cycles 2 --duration 8` | **PASS** | Cycle 1: `video=205`, `audio=356`, `frames_written=62560`. Cycle 2: `video=194` (`28.31 fps`), `audio=354`, `frames_written=124160`, `alsa_errors=0`, `underruns=0`, `TX sco +2058` (124 B/pkt), `RX sco +2970`, `0` SDIO crashes. |
| **10-Cycle Talk ON/OFF Stress** | 10 rapid `/api/talk?active=0` / `active=1` toggles during active WebRTC session | **PASS** | `talk_off_events=10`, `talk_on_events=10`, `device_open_count=10`, `device_close_count=9`, `alsa_errors=0`, `underruns=0`, `video=176` (`28.54 fps`), `audio=377`. |
| **Controlled BT Disconnect & Recovery** | `scripts/standalone/test_bluetooth_recovery.sh` + post-recovery `test_device.py` | **PASS** | Reconnected ACL + `PROFILE=sco` (`SCO MTU: 120:4`) in **7s** with unchanged media PID (`3070`); post-recovery WebRTC test passed (`video=165` at `29.29 fps`, `audio=369`, `frames_written=224960`, `alsa_errors=0`, `0` SDIO crashes). |

---

## 5. Remaining Risks and Unresolved Hypotheses

- **AIC8800 SDIO Driver Terminal Crash State**: If external RF interference or kernel SDIO bus contention ever causes an HCI command to exceed the 2-second timeout in `aic8800_fdrv` (`rwnx_cmds.c`), the kernel driver sets `RWNX_CMD_MGR_STATE_CRASHED`, which cannot be cleared without reloading the kernel module or rebooting the board. Eliminating periodic HCI/D-Bus churn prevents triggering this bug during normal operation.
- **Stage-Specific `LD_PRELOAD` Path in `/etc/init.d/S41bluealsa`**: Standalone deployments that change the staged directory path rewrite `LD_PRELOAD=$shim` in `/etc/init.d/S41bluealsa` and restart `bluealsa`, which briefly drops active Bluetooth connections until `bt-watch` reconnects (~5–7s).

---

## 6. Rollback Procedure

To roll back the deployed standalone stage on the device over USB SSH:
```bash
ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261110-161225-051d5771-db25ef48/rollback.sh'
```

To revert the repository source changes locally:
```bash
git checkout -- src/g4c4_bt_capture.hpp src/g4c4_bt_playback.hpp src/maix_webrtc_maixcam.cpp target_services/webrtc_service.sh scripts/standalone/test_bluetooth_recovery.sh
rm -f scripts/tests/test_sco_duplex_repair.py
```

---

## 7. Final Hardware Acceptance Status

- **Audible playback confirmed by user on earbuds**: **PASS** (Confirmed in Experiment C with `SCO MTU: 120:4`).
- **Simultaneous microphone capture**: **PASS** (`354–377` Opus uplink packets per 8s test cycle).
- **Zero new SDIO command queue crashes**: **PASS** (`dmesg | grep -c "cmd queue crashed"` remained `0` across all post-reboot experiments and stress tests).
- **Stable SCO RX/TX behavior**: **PASS** (`RX sco: 67865`, `TX sco: 6455` at 120-byte SCO payload framing, `errors: 0`).
- **No repeated capture reset storm**: **PASS** (`0` capture resets while connected).
- **No unexpected HFP reconnection loop**: **PASS** (`0` ACL/ConnectProfile churn while `PROFILE=sco` is present).
- **Working RTSP/WebRTC video**: **PASS** (`28.3–29.7 fps` H.264 `640x480`).
- **Repeated Talk ON/OFF cycles**: **PASS** (`10/10` cycles clean, `0` ALSA errors/underruns).
- **Recovery after controlled Bluetooth disconnect**: **PASS** (Auto-recovered `PROFILE=sco` at `120:4` in `7s` and passed full-duplex media verification).
