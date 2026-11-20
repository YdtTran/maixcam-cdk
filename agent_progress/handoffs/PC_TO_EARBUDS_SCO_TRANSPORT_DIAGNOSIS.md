# SCO transport investigation

Status: **unresolved; baseline captured, physical listening verdict pending**.

## New baseline

After user authorization, media and Bluetooth reconnection were restored. A synthesized 15-second WAV containing the requested sentence, 8 kHz mono S16_LE, SHA-256 `3c60551adbb588d27ca59f5b225c14963f33253cc1f162b42ddf0e73b086c23c`, was sent through direct SCO with media capture stopped. The speech itself is approximately eight seconds followed by silence. `aplay` exited 0 and reported no underruns. `btmon` recorded 944 outgoing 254-byte SCO HCI payloads; incoming: valid 1015, possibly invalid 416, no-data 611. Synchronous connection completion reported interval 0x0c, retransmission window 0x02, RX/TX air lengths 60, CVSD. A sampled BlueALSA system call was `write(..., 254) = 254`, 0.703 ms; full SCO-specific syscall distribution, socket MTU and timing distribution were not extracted. The bounded HCI trace and strace were removed after summary extraction. The user requested a repeat; the repeat also exited 0. **Listening verdict for both runs: NOT VERIFIED.** Media and network were subsequently confirmed running.

Subsequent live socket inspection found BlueALSA SCO descriptors with BT_VOICE `0x0060` and socket MTU 255. A three-second read-only strace, with payloads suppressed, found 398 reads returning 120 bytes each on the active receive descriptor. This is a host socket read size, distinct from the negotiated 60-byte air packet length.

An experimental preload (`scripts/sco_chunk_probe.c` and `scripts/sco_chunk.h`) precedes the existing alignment preload. It requires a same-process 120-byte SCO read before sending, splits CVSD writes into even chunks, paces parts according to 16,000 PCM bytes per second, propagates errors without retrying an already-sent prefix, and logs only metadata. Host checks cover byte order, even short writes, errors, odd-byte alignment and unchanged unrelated sockets.

The first 120-byte probe run ended after roughly half a second: 48 writes of 120 and 23 writes of 14 succeeded, followed by a 14-byte write failing `ENOTCONN` (errno 107); `aplay` reported `No such device` and exit 1. HCI recorded 48 120-byte and 23 14-byte TX packets (median TX interval 7.501 ms; maximum 7.590 ms); RX status valid 8, possibly invalid 0, no-data 23, partially lost 0. Link parameters were CVSD, interval 0x0c, retransmission window 0x02, air RX/TX length 60. The listener was unsure whether any sound was heard. This failed run does not identify a cause: restarting BlueALSA, link reconnection, packet sizes and pacing were changed together. The probe currently emits a 14-byte remainder after two 120-byte chunks, so this is not a pure fixed-120 packet test.

The first run exposed a restoration race: the runner briefly counted the exiting diagnostic BlueALSA process as the restored daemon. The original daemon was manually restarted; its D-Bus endpoint and SCO/A2DP endpoints were verified, and network/media were running. The runner was modified to reject diagnostic-marked daemons, check the expected alignment preload and D-Bus response, and verify that the service remains present.

The restart-only control then failed before playback: ALSA could not install hardware parameters and no SCO packets were observed. The runner reported that BlueALSA was not restored; its watchdog remained running. The resumed supervisor recovered the original BlueALSA and media. A separate check confirmed the original alignment preload, D-Bus response, both SCO endpoints, A2DP endpoint and network, after which the leftover watchdog and its temporary directory were removed. There is no valid audible or HCI comparison from this control. The preflight incorrectly accepted an A2DP PCM as sufficient; it now requires the SCO playback PCM. Restoration reliability still needs further work before another disruptive test.

| Run | PCM | SCO TX | SCO RX status (valid / possibly invalid / no data / partially lost) | Player | Listening |
|---|---|---|---|---|---|
| Existing BlueALSA baseline | Fixed 15-second WAV | 944 × 254 bytes | 1015 / 416 / 611 / 0 | Exit 0 | NOT VERIFIED |
| 120-byte split probe | Same WAV | 48 × 120 bytes, 23 × 14 bytes | 8 / 0 / 23 / 0 | Exit 1; socket `ENOTCONN` | User unsure; NOT VERIFIED |
| Restart-only control | Same WAV | 0 | 0 / 0 / 0 / 0 | Exit 1 during PCM setup | NOT VERIFIED; playback did not start |

The bounded runner is `scripts/sco_experiment.py`; it has baseline, restart-control and chunk modes, uses a separate 90-second recovery watchdog, and deletes raw capture files on normal completion. It must be copied with `sco_trace_summary.py` to the device; the chunk mode also needs a separately cross-compiled `sco_chunk_probe.c` library and the WAV fixture. `scripts/sco_make_fixture.py` reproduces the fixture using Flite and rejects nonmatching output; the runner verifies the fixture hash. Host tests now stop a real marked dummy process, resume a real SIGSTOP-paused process and simulate delayed D-Bus release and recovery errors. The revised device recovery has not been validated against actual BlueALSA/D-Bus failure; **do not run another disruptive experiment until that gap is addressed**.

Read-only PipeWire feasibility check: the Buildroot 2023.11.2 RISC-V device has no `pipewire`, `wireplumber`, `pw-cli`, PipeWire libraries or SPA directory in the checked locations. A reversible test would require a compatible cross-built runtime and dependencies plus careful exclusive BlueZ profile ownership. No evidence yet justifies installing or migrating it.

## Running configuration at initial audit

USB SSH `10.172.16.1` is reachable; `192.168.1.8` timed out. The state file says `READY`, but it is stale: no `stream_webrtc-m2` process runs. BlueALSA 4.1.1 PID 498 runs with A2DP source and HFP audio gateway, and has the existing `libg4c4_sco_write_align.so` preloaded. No BlueALSA PCM endpoints were listed because the earbuds are connected to the PC. PIDs 507 and 886 (`bt_event_manager.sh`) and 1172 and 2414 (`webrtc_autostart_daemon.sh`) are SIGSTOP-paused; `/run/pc-earbud-test.json` records them. Verify identities again before resuming; do not use the stale state file as proof of readiness.

The configured release executable SHA-256 begins `9d6a13d43d31c08b` and the loaded alignment shim begins `db25ef484e32c686`. BlueALSA SHA-256 begins `591d6dff462f52cd`. The release is configured in `/root/webrtc_service.sh`, but its executable is not currently running.

After the audit, media was restarted and the paused processes were resumed. The earbuds reconnected; subsequent checks confirmed a running media process, network status and both SCO PCM endpoints. Recheck current state before any test.

## Evidence and data path

Direct playback is ALSA BlueALSA PCM → BlueALSA CVSD encoder thread → SCO socket → Linux HCI → AIC SDIO driver/firmware → radio. BlueALSA manages playback and capture in separate transport threads; both BlueALSA and PipeWire drain incoming SCO even without an application capture client. The existing preload intercepts odd-sized CVSD SCO `write()` calls and makes them even, preserving whole S16 samples. BlueALSA controls its CVSD packet size from the socket MTU and paces by samples; PipeWire 1.4.2 sizes writes using the smaller of its write MTU and received packet size. See [source comparison](SCO_SOURCE_COMPARISON_2026-11-10.md) and [PC comparison](PC_VS_MAIX_SCO_COMPARISON_2026-11-10.md).

Prior direct Maix SCO playback was silent to the user; the same source on PC CVSD was clear. A prior Maix trace had source-exact HCI TX and RX statuses: valid 100, possibly invalid 212, no data 1727, total 2039. This does not prove outgoing air delivery or identify the failed layer. Quiet-Wi-Fi experiments have no reported listening verdict. PC `btmon` lacked permission, so its SCO packet sizes and link negotiation remain unknown.

## Prioritized experiment matrix

| Priority | Experiment | Measurement / rationale | Operational risk |
|---|---|---|---|
| 1 | Restore Maix media and reconnect earbuds | Confirm actual process, PCM endpoints, network and service readiness | Temporary PC Bluetooth disconnect and stream restart |
| 2 | Isolated baseline, existing BlueALSA and shim | Bounded write return/size/MTU timestamps and HCI metadata, ALSA errors and listener verdict | Temporary media interruption; ensure independent watchdog restores service |
| 3 | RX-informed even packet sizing | Use only measured, validated sizes; preserve byte order, sample count, short-write semantics and sample-rate pacing | Temporary isolated BlueALSA instrumentation; do not alter service configuration |
| 4 | One bounded alternative derived from measured socket behavior | Compare timing and listener verdict with identical PCM | Same as above; abort on errors or unsupported writes |
| 5 | Working PC cross-check | Obtain socket options, HFP state, HCI link and timing with appropriate privilege | PC capture permission currently unavailable |
| 6 | Evaluate PipeWire on Maix | Check executable, dependencies and profile ownership in a disposable environment | Avoid migration absent supporting evidence |

Use one approximately 15-second prerecorded fixture containing the requested exact sentence for all new tests, verify its hash and 8 kHz mono S16 format. Record the physical listening outcome separately for each run. Bounded HCI traces contain microphone audio: extract packet metadata and remove traces. Preserve separate valid, possibly invalid, no-data and other status counts. Save sizes, returns, timestamps, negotiated connection, ALSA errors and underruns. Do not equate air packet length with host PCM length.

## Restoration procedure and gating

Restoring requires disconnecting the earbuds from the PC, confirming the recorded PIDs still have the expected command lines, resuming them, and checking that media PID, network, Bluetooth endpoints and supervisor health are actually present. If automatic recovery fails, use the established service scripts and verify the result. A separate bounded watchdog must resume processes and restore services if an experiment or SSH session fails. Preserve the current shim, A2DP configuration, Wi-Fi and USB SSH. Avoid firmware, kernel and controller parameter changes.

The user authorized the investigation and disconnected the earbuds from the PC. `scripts/sco_capture_metadata.py` and `scripts/sco_trace_summary.py` provide bounded, metadata-only trace analysis; run them together on the device as root, e.g. `python3 sco_capture_metadata.py --seconds 20`. Temporary raw traces may contain microphone audio and are deleted after normal completion, exceptions and SIGTERM; uncatchable termination or power loss can leave a private temporary directory to remove. This script observes only; it does not isolate media or alter SCO writes. A two-second normal-service run measured 265 RX packets of 120 bytes (valid 232, possibly invalid 14, no data 19, partially lost 0), median gap 7.5015 ms and maximum gap 10.881 ms; this is not a playback experiment. No packet-sizing change has yet been created. Do not claim a fix until counting speech is audible over Maix SCO.
