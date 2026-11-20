# PC versus MaixCAM SCO comparison — 2026-11-10

Branch: `test/sco-capture-wifi-coexistence`.

The user confirmed clear counting from the PC using the same prerecorded PCM as MaixCAM. Direct MaixCAM SCO playback completed but the user reported silence. This establishes a host-dependent failure; it does not yet distinguish userspace, kernel, controller firmware, or radio/link setup.

## Measured comparison

| Item | PC | MaixCAM | Meaning |
|---|---|---|---|
| Earbuds | EDIFIER X2 Plus, `6C:16:29:7B:00:D4` | Same earbuds | Earbuds support audible SCO with another host. |
| Source PCM | Counting fixture copied from MaixCAM | `/root/maixcam-tests/bluetooth-sco/count-60s.raw` | Both full raw files have SHA-256 `e4627f1c72e939e58570694242bc05ce155e8ef7cacf104064174d3e463f0f3f`; PC played a 20-second WAV slice. |
| PCM / codec | S16_LE, 8000 Hz, mono; CVSD | S16_LE, 8000 Hz, mono; CVSD | Matching format and codec; not an mSBC-versus-CVSD comparison. |
| Result | User: “very clear for pc”; `pw-play` exited 0 | User: “silience”; direct `aplay` exited 0 | Process success alone is insufficient evidence of sound. |
| Controller | Realtek USB `0bda:4853`, `btusb`; Bluetooth 5.2 | AIC/RivieraWaves SDIO; Bluetooth 5.2 | Different transport, controller and firmware; Bluetooth version itself is equal. |
| Kernel | `6.12.115+deb13.1-amd64` | `5.10.4-tag-`, RISC-V | Kernel SCO handling differs; version difference alone proves no bug. |
| Bluetooth userspace | BlueZ 5.82; PipeWire 1.4.2; WirePlumber 0.5.8 | BlueZ 5.68; BlueALSA 4.1.1 | Different SCO setup, packet sizing and scheduling paths. |
| Advertised SCO capacity | `255:12` | `255:4` | Same advertised maximum payload; different controller buffer counts. Neither proves actual packet length or a queue failure. |
| Application capture during direct playback | No explicit recording client launched; PipeWire manages the SCO transport | WebRTC stopped; no `arecord` | Maix silence does not require application microphone capture. SCO RX may still exist without an application capture client. |
| Volume | SCO sink unmuted, volume 0.40 | SCO playback 15/15, unmuted | Maix silence is not explained by its inspected mixer setting. |
| A2DP | Not auditioned in this PC comparison | User confirmed audible counting, though player timed out | Maix can deliver audible Bluetooth audio; failure differs by profile. |
| Reduced Wi-Fi | Not controlled | Two direct SCO runs completed with wlan0 down and USB SSH intact | User did not report audibility for these two runs; coexistence is not ruled out. |

Local evidence commands: `uname -r`, `bluetoothctl --version`, `hciconfig hci0`, `hciconfig hci0 version`, `lsusb`, `wpctl inspect 70`, `pw-cli enum-params 69 Profile`, `pw-cli enum-params 70 Format`, `sha256sum`; corresponding device commands over USB SSH. PipeWire object IDs are session-specific.

## Concrete implementation difference

PipeWire 1.4.2 [sco-io.c](https://github.com/PipeWire/pipewire/blob/1.4.2/spa/plugins/bluez5/sco-io.c) waits for an RX packet and writes blocks sized to the minimum of the socket write MTU and received packet length. It also keeps draining RX without a connected source client. Its [sco-sink.c](https://github.com/PipeWire/pipewire/blob/1.4.2/spa/plugins/bluez5/sco-sink.c) uses this helper in the CVSD path as well as encoded paths.

BlueALSA 4.1.1 [sco.c](https://github.com/arkq/bluez-alsa/blob/v4.1.1/src/sco.c) computes the CVSD sample count from the write MTU and sends MTU-sized writes. MaixCAM loads `src/g4c4_sco_write_align.c`, which reduces odd S16 SCO writes by one byte. That workaround fixes sample alignment, not adaptive packet sizing or controller behavior. Earlier Maix traces recorded 254-byte outgoing PCM payloads; PC outgoing packet length was not measured.

BlueALSA also drains incoming Bluetooth data before checking whether its capture PCM is active; RX draining alone is not a distinguishing explanation. The packet-size selection is the useful difference.

This is a testable software difference, not proof that 254-byte writes are unsupported. Do not compare the 60-byte negotiated air packet length directly with host PCM payload length.

## Earlier MaixCAM boundary evidence

The existing [diagnosis handoff](PC_TO_EARBUDS_DIAGNOSIS_2026-11-10.md) records source-exact outgoing PCM at the Linux HCI boundary, with regular host-side timing. Of 2039 incoming SCO packets, 1727 had no data and 212 were marked possibly invalid; only 100 were marked valid. This older tone experiment is separate from today's counting comparison. It suggests a synchronous-link/controller issue, but does not prove outgoing audio was delivered over the air or identify which side caused the RX failures. HCI aggregate `errors:0` is not evidence that SCO packet status is healthy.

The same handoff records an earlier controller command-queue stall cleared by reboot without restoring audible SCO. A historical stall is not proof that the current controller is stalled.

## Ranked leads and decisive comparisons

1. **Maix controller/firmware/SDIO SCO handling or link negotiation.** Supported by source-exact host TX plus mostly missing/invalid RX in the earlier trace. Compare fresh connection-complete parameters and packet status on both hosts; a known-good USB Bluetooth controller on Maix, if supported, would isolate the onboard path further.
2. **BlueALSA packet sizing/pacing or profile setup versus PipeWire.** There is a specific implementation difference. Run BlueALSA 4.1.1 with the alignment fix on the PC controller, isolating it from PipeWire first; alternatively conduct a reversible packet-size experiment on Maix using observed RX sizes. Do not install a second competing profile owner without isolating the existing one.
3. **Radio coexistence or stale profile state.** Still possible, but quiet-Wi-Fi audibility was never reported. A fresh isolated SCO connection with verified release of the old link is needed before attributing the fault to capture or coexistence.

## Limits and current device state

- PC `btmon` failed with `Operation not permitted`; passwordless sudo is unavailable. PC HCI packet lengths, negotiated interval/window and packet-status statistics remain unmeasured.
- No source or persistent device configuration changed during this comparison. No driver or firmware replacement attempted.
- Earbuds remain paired, trusted and connected to the PC in CVSD hands-free mode.
- MaixCAM media is stopped. Its supervisor and Bluetooth reconnect processes remain SIGSTOP-paused to prevent reclaiming the earbuds. `/run/pc-earbud-test.json` records PIDs `[507, 886, 1172, 2414]`; verify process identity before resuming. Wi-Fi and USB networking remain available. Return the earbuds to Maix and resume these processes when the PC comparison ends.
