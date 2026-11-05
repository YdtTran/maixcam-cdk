# PC → EDIFIER playback: session handoff

## Resume here

Fix audible **PC microphone → MaixCAM → EDIFIER X2 Plus earbuds** playback while preserving the working **earbud microphone → MaixCAM → PC** stream. The user explicitly confirmed that playback should come from the EDIFIER earbuds, not the MaixCAM speaker/jack.

**The playback issue is unresolved.** Builds, packet counters, successful ALSA writes, and correct HCI PCM have repeatedly passed while the user still hears silence. Completion requires the user to hear actual PC speech, with the reverse microphone stream still working.

The user also requested removal of all software audio filters. Keep the current raw-audio behavior unless the user changes that requirement. The latest request was to write this handoff, so diagnostic work has stopped.

First actions for the next session:

1. Read the repository `AGENTS.md` and inspect git status/history. Changes below were prepared since the base commit and may have been committed after this handoff was written; preserve any remaining uncommitted work.
2. Reconfirm device connectivity and service state using the commands below.
3. Establish a reliable listening baseline before changing more code. Prior direct-beep answers are inconsistent; see the experiment table.
4. Compare standalone playback, simultaneous capture/playback, and the application using the same known PCM and connection state. Observe actual SCO connection lifecycle and RFCOMM/volume changes, not just successful writes.

## Current deployment and access

Verified immediately before writing this document:

| Item | Current value |
| --- | --- |
| Repository | `/home/ydt/Documents/Workspace/maixcam-cdk` |
| Base commit | `35a9548` |
| Device service state | `STATE=READY` |
| Active release | `/root/maixcam-redeploy-20261105-124452-9d6a13d4-db25ef48` |
| App SHA256 | `9d6a13d43d31c08bafbb3681517510c4cfea85340b82a25811d85a8991ece88b` |
| Alignment shim SHA256 | `db25ef484e32c686a9b6cf44511c31963e439ec16290d8a3fa064bc7ee9a3eea` |
| Device service | `/root/webrtc_service.sh` |
| Supervisor | `/etc/init.d/S99webrtc_autostart`, `/root/webrtc_autostart_daemon.sh` |
| Runtime log | `<active release>/m2_runtime.log` |
| Earbuds | EDIFIER X2 Plus, `6C:16:29:7B:00:D4` |
| Bluetooth daemon | BlueALSA `v4.1.1`, profiles `a2dp-source` and `hfp-ag` |
| Hardware | AIC SDIO Bluetooth, manufacturer reported as RivieraWaves (96), HCI 5.2 |
| Controller SCO MTU | Original **255:4**; experimental 120-byte setting was reverted |
| Playback mixer | SCO playback 12/15 (80%), unmuted; capture 15/15, unmuted |

The app is running and no diagnostic `aplay`/`arecord` processes remained at handoff. The last diagnostic restored the service and supervisor. `/tmp` was 3% used, with the unbounded debug WAV absent.

Wi-Fi SSH at `192.168.1.8` timed out. USB recovery SSH works at `root@10.172.16.1`. Follow `AGENTS.md` for authentication and recovery; do not ask the user to supply credentials again. This session used an SSH control master:

```sh
ssh -S /tmp/maixcam-redeploy-ssh root@10.172.16.1 \
    'cat /run/webrtc_autostart.state; head -10 /root/webrtc_service.sh'
```

The control master may expire. Reestablish interactive SSH if necessary. The device also advertises `10.172.17.1` and AP address `192.168.4.1`; multiple host interfaces sometimes cause ICE source-address mismatches in tests.

HTTPS/WSS are ports 8000/8001. The trusted public CA certificate is available locally at `build/standalone-secrets/ca.crt` (ignored by git). The service reads its signaling token from `/etc/maixcam-standalone/signal-token`. Keep token/private-key contents out of outputs and handoffs.

## What the user has actually reported

The user repeatedly says **Maix → PC always works**, while **PC → earbuds is silent or noisy**. They reported massive noise with the original 255-byte behavior; changing controller MTU to 120 also produced noise and was reverted.

Latest listening results:

| Test | Setup | User response |
| --- | --- | --- |
| Direct low beep then high beep | App/supervisor paused; `aplay` 8 kHz mono SCO, then 48 kHz stereo A2DP | Initially **“Both beeps”**; user then asked to repeat |
| Repeated direct pair | Same script; service restored afterward | No separate, explicit result for this repetition |
| WebRTC sine tone | Python aiortc client, 6 seconds, real Opus downlink | **“No tone”** |
| Longer WebRTC sine tone | 15 seconds; confirmed active before asking | **“Silent”** |
| Direct SCO beep with capture already active | `arecord` 48 kHz mono first, then `aplay` native 8 kHz mono | **“Silent”** |
| Direct SCO beep with native capture | Same, but `arecord` at 8 kHz mono | **“Silent”** |
| Playback first, then capture after 4 seconds | 12-second SCO beep; start 48 kHz capture after 4 seconds | **“Never heard beeps”** |

**Important uncertainty:** the initial “Both beeps” response arrived while a test was still running, and later “Never heard beeps” contradicts a simple claim that capture alone causes silence. Earlier commentary overstated the capture-dependent conclusion. Treat startup order, duplex interaction, and the standalone baseline as hypotheses, not proven causes. Ask listening questions only after confirming that a sufficiently long tone is actually playing.

The user closed the MaixCAM webpage when asked so a test client could own WebRTC. It may now be open again. Only one signaling owner is permitted. A test receiving `{"type":"busy"}` did not test playback and must not be reported as a media pass.

## Strong technical observations

- PC packets arrive, Opus decoding succeeds, and playback writes progress with no sustained underruns/errors. This has **not** established audibility.
- During the 15-second Python WebRTC tone, HCI monitoring captured outgoing **254-byte** SCO payloads. A two-second PCM segment had peak 13409, RMS 8487.4, and **100.0% energy at the expected 440 Hz**. The correct test tone reached the host/controller boundary while the user heard silence.
- A standalone 8 kHz `aplay` tone was also captured at HCI. A 1000-byte non-silent window spanning several packets matched the original WAV PCM exactly.
- Current controller MTU remains 255. The software shim sends 254-byte CVSD writes to preserve whole S16 samples.
- `hciconfig hci0 voice` reports global/default `0x0000` (8-bit, one's complement). **Do not infer the active socket is using that format.** A read-only diagnostic using `pidfd_getfd` and `getsockopt` found active SCO sockets configured as **BT_VOICE `0x0060`**, MTU 255. The global value alone is not evidence for changing controller voice configuration.
- Some SCO fds observed in BlueALSA are duplicated thread descriptors; their count alone does not prove multiple independent links.
- Removing only the A2DP profile did not restore sound. That operation briefly disturbed HFP capture, which recovered via the watcher. A2DP was subsequently reconnected for the direct two-tone test. Avoid repeating this as an assumed fix.
- Simulated HFP call signals (`+CIEV:1,1`, `+CIEV:5,5`, `+CIEV:2,1`) were tried, including through an interactive `bluealsa-rfcomm` PTY. The user still reported silence. This is not automated/persisted; later daemon restarts reset state.
- Changing playback from 48 kHz stereo to native 8 kHz mono passed regression tests but did not restore audible PC speech.

## Changes already in the worktree

### Earbud microphone and Bluetooth service

- `src/g4c4_bt_capture.hpp`: nonblocking BlueALSA SCO capture, requesting 48 kHz mono S16 through ALSA conversion. Lazy open, close/reset, and retry support.
- `src/maix_webrtc_maixcam.cpp`: `MAIX_WEBRTC_AUDIO_SOURCE=mic` now captures earbuds rather than `audio::Recorder`/onboard ADC. Retries Bluetooth failure without stopping video; no fallback to the MaixCAM mic.
- `target_services/webrtc_service.sh`: enables HFP AG if necessary and starts a `bt-watch` subprocess that explicitly calls BlueZ `ConnectProfile` for Handsfree every five seconds. Generic Bluetooth Connect sometimes restored only A2DP.
- Deployment scripts now install the versioned service template. Standalone TLS/AP settings remain enabled.

### Raw audio

- `src/maix_webrtc_audio_dsp.hpp` retains its compatibility class name but is now only a bounded PCM frame accumulator. Samples are copied unchanged; no HPF or Speex processing exists.
- `/api/audio_dsp` GET reports `mode=bypass`, `filters_removed=true`, HPF/Speex false. POST returns 410, preventing re-enabling removed filters.
- `src/client.js` sends the original microphone track, with browser echo cancellation, noise suppression, and AGC all false. Talk toggling still swaps between microphone and a silent track.
- Removed noise-gate worklet source/embedded asset, filter UI controls, Speex build/link dependency/archive, and obsolete filter tests. Updated HTML is embedded in `src/maix_webrtc_http_server.hpp`.
- Browser recordings/vault functionality remains separate and was not removed.

### Playback and resource fixes

- `src/g4c4_bt_playback.hpp`: chooses 8 kHz mono for `PROFILE=sco`, retaining 48 kHz stereo for explicit A2DP. `enqueue_pcm()` uses configured channel count.
- `src/g4c4_opus_rx.hpp`: feeds decoded 8 kHz mono directly to SCO; resampler is bypassed for HFP. Also removed an unconditional debug recorder to `/tmp/downlink_pc_to_maix.wav`.
- That old debug recorder filled the device's entire 63.9 MiB `/tmp` after several minutes. It caused failed/truncated tone uploads and is now removed. Do not restore unbounded recording or treat this resource fix as proof that playback is fixed.
- `src/maix_webrtc_server.hpp`: playback telemetry includes `sample_rate` and `channels`.

### BlueALSA compatibility shim

`src/g4c4_sco_write_align.c` builds `build/dl_lib/libg4c4_sco_write_align.so`.

Upstream BlueALSA 4.1.1 `src/sco.c` computes `mtu_samples = mtu_write / sizeof(int16_t)`, writes `mtu_write` bytes, then advances by `mtu_samples`. For MTU 255 this sends 255 bytes but consumes 254, repeating a byte across packet boundaries. The shim intercepts only odd-length `write()` calls on Bluetooth SCO sockets with CVSD 16-bit voice setting, issuing a genuine even-length short write. Other sockets/codecs and all capture reads remain untouched.

The regression reproduces the duplicated-byte pattern before the fix and passes with the shim, including on target. HCI confirms whole-sample packets afterward. **This fixes a real byte-alignment defect but has not solved the audible failure.**

Only BlueALSA is preloaded, via `/etc/init.d/S41bluealsa`:

```text
LD_PRELOAD=<active release>/dl_lib/libg4c4_sco_write_align.so start-stop-daemon -S ...
```

The service installs this hook only for BlueALSA 4.1.1. Build/deploy scripts include the shim, and release names include its hash to avoid overwriting a mapped library in place. Backups include `S41bluealsa.before-sco-align`; root also has earlier diagnostic backups. Inspect before restoring anything.

## Build, tests, and evidence

The SDK/toolchain were missing initially and are now installed under `/home/ydt/maixcam-build`:

- SDK: `MaixCDK-opus-g4a`, official Sipeed checkout, SDK example dependencies built.
- Compiler: SDK `dl/extracted/toolchains/maixcam/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-g++`, Xuantie GCC 10.2.0.
- Opus: `opus`, tag v1.5.2, archive in `opus/build-maixcam`.
- Test environment: `/home/ydt/maixcam-build/venv/bin/python3` with aiortc, aiohttp, and PyAV.

Normal rebuild works:

```sh
python3 scripts/update_http_server_html.py
python3 scripts/build.py
```

New regression sources:

| File | Scope/result |
| --- | --- |
| `scripts/tests/test_raw_audio.cpp` | Exact PCM passthrough across chunk boundaries; passed |
| `scripts/tests/test_raw_audio_client.js` | Actual client JS with browser stubs: direct mic, processing constraints false, Talk toggling; passed |
| `scripts/tests/test_sco_write_align.c` | Actual interposed writes with emulated SCO metadata; failed before shim, passed after, host and target |
| `scripts/tests/test_sco_playback.cpp` | Real Opus encode/decode through receiver with fake ALSA sink; exact native PCM, 8 kHz mono, no resampling; failed old format, passed new |

Example native tests:

```sh
node scripts/tests/test_raw_audio_client.js
g++ -std=c++17 -pthread -Isrc scripts/tests/test_raw_audio.cpp -o /tmp/raw-audio-test
/tmp/raw-audio-test
cc -rdynamic scripts/tests/test_sco_write_align.c -o /tmp/sco-align-test
cc -shared -fPIC src/g4c4_sco_write_align.c -ldl -pthread -o /tmp/sco-align.so
LD_PRELOAD=/tmp/sco-align.so /tmp/sco-align-test
```

The playback test also needs SDK ALSA headers, Opus headers, and host `libopus.so.0`/`libasound.so.2`; its previous executable is `/tmp/test-sco-playback`.

Real WebRTC probe (requires the user's page closed):

```sh
/home/ydt/maixcam-build/venv/bin/python3 scripts/standalone/test_device.py \
  --target 10.172.16.1 --duration 15 --cycles 1 \
  --output agent_progress/standalone/evidence/webrtc-tone-long.json
```

This sends a continuous 440 Hz tone. Existing tests assert packet/frame receipt, **not audible playback**. Reports saying PASS must not be interpreted as solving the user's issue.

Local diagnostic material is under `/home/ydt/maixcam-build/debug-audio/`:

- `play-direct-tests.sh`: stop supervisor/media, SCO low tone, A2DP high tone, restore via EXIT trap.
- `play-duplex-tone.sh`: capture 48 kHz first, then 12-second SCO tone.
- `play-duplex-native-tone.sh`: same with 8 kHz capture.
- `playback-first-tone.sh`: tone first, microphone after four seconds.
- `play-aligned-tone.sh`, `read-sco-options.c`, source clone `bluez-alsa/` at v4.1.1.
- WAV fixtures `sco-tone.wav`, `a2dp-tone.wav`, `duplex-tone.wav`.
- `webrtc-tone-check.log`, `webrtc-tone-long.log`.

Scripts are local debug tools, not generalized deployment commands. Read them before running. They use an EXIT trap to restore media/supervisor; do not stop the standalone network or USB SSH. Device tone fixtures currently exist in `/tmp`. Monitor traces can contain room audio; keep them private and remove them after analysis. SDP/evidence files are ignored and may contain ephemeral ICE credentials.

Release/deploy helper files outside the repo:

- `/home/ydt/maixcam-build/redeploy-current.json`: active stage and hashes.
- `/home/ydt/maixcam-build/redeploy-current.sh`: most recent guarded deployment.
- Older `deploy-*.sh` helpers target earlier stages; inspect them rather than blindly reusing.

## Next diagnostic boundary

Reconfirm a direct SCO-only beep **after all media clients have closed and the previous SCO link has actually been released**. Record HCI connection handles/lifecycle and RFCOMM volume events. Then compare the same PCM with capture active. The last playback-first result means the earlier capture-only explanation is not yet established.

Keep the working earbud capture configuration, raw-audio requirement, and original controller packet setting intact while narrowing the fault. A new session should not start by repeating redeploys or claiming that counters prove sound. The next useful result is a reproducible audible/non-audible comparison with synchronized user feedback.
