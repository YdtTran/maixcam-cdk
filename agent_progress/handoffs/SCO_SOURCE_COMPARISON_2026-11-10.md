# SCO source comparison — 2026-11-10

Source inspection only; no device mutations. PC clear playback versus MaixCAM silent playback is user-observed; software/controller version differences alone do not identify a cause.

| Boundary | PC source path | MaixCAM source path | What this establishes |
|---|---|---|---|
| CVSD host data | PipeWire SCO sink sends unencoded PCM in the branch following mSBC/LC3 encoding, through shared `spa_bt_sco_io_write` | BlueALSA `sco_cvsd_enc_thread` writes PCM directly, with sample-count pacing | Matching 8 kHz mono S16 input is expected on both; CVSD encoding belongs downstream in controller |
| Socket packet size | Shared I/O waits for first received packet and writes `min(write_mtu, read_size)` bytes | BlueALSA obtains SCO_OPTIONS MTU; only USB gets a fixed CVSD 48-byte adjustment. SDIO keeps socket MTU. Encoder writes that MTU | Concrete difference worth controlled comparison; actual PC packet size has not been captured |
| RX draining | PipeWire reads even without source callback | BlueALSA decoder reads BT before testing whether PCM client is active | Both have RX draining logic; absence of capture client does not by itself distinguish them |
| Kernel transport | Linux 6.12 btusb uses isochronous URBs for SCO, selects USB alternate interface on connection notification | AIC btsdio enqueues commands/ACL/SCO in same queue, prepends HCI packet type, forwards through `rwnx_sdio_bt_send_req`; this uses firmware message plus confirmation | Substantially different scheduling/firmware boundary below otherwise equivalent PCM |
| Routing setup | USB has explicit isochronous endpoint/interface setup in driver | BSP CONFIG_SDIO_BT chooses BTPORT_MB rather than UART; BSP patches btmode/btport into firmware config. BlueALSA has Broadcom-only PCM-to-transport routing correction | Mailbox selection is not proof of AIC SCO/PCM audio-route setting; no AIC-specific SCO route command established by inspected sources |
| Envelope size | USB URB construction/endpoint sizes govern transport | `mm_bt_send_req.bt_data[1024]`; observed 254 payload + 3 SCO header + 1 packet-type bytes fits | No evidence for simple overflow of this 1024-byte mailbox buffer |

## Primary references

- [PipeWire 1.4.2 SCO sink](https://github.com/PipeWire/pipewire/blob/1.4.2/spa/plugins/bluez5/sco-sink.c#L503): lines503–565 show mSBC/LC3 branch and plain PCM branch calling shared I/O. This verifies the RX-derived sizing applies to CVSD too.
- [PipeWire 1.4.2 shared SCO I/O](https://github.com/PipeWire/pipewire/blob/1.4.2/spa/plugins/bluez5/sco-io.c#L34): rationale at34–42, RX at83–114, write sizing at148–179.
- [PipeWire 1.4.2 native backend](https://github.com/PipeWire/pipewire/blob/1.4.2/spa/plugins/bluez5/backend-native.c#L2448): SCO_OPTIONS-derived initial MTU and fallback; transparent setting is specifically applied for mSBC/LC3.
- [Linux 6.12 btusb](https://github.com/torvalds/linux/blob/v6.12/drivers/bluetooth/btusb.c#L1957): SCO isochronous submission; connection notification and alternate-interface handling at1979–2141.
- [BlueALSA v4.1.1 hci.c](https://github.com/arkq/bluez-alsa/blob/v4.1.1/src/hci.c#L119): socket MTU and USB correction. [sco.c](https://github.com/arkq/bluez-alsa/blob/v4.1.1/src/sco.c#L224): CVSD encoder and decoder; Broadcom route correction at176–200. [transport](https://github.com/arkq/bluez-alsa/blob/v4.1.1/src/ba-transport.c#L882): CVSD BT_VOICE_CVSD_16BIT and socket MTU acquisition.
- [Sipeed AIC source at inspected commit](https://github.com/sipeed/LicheeRV-Nano-Build/tree/a4d8622406de75bc35c4e01207f23e9497bff5f0/osdrv/extdrv/wireless/aic8800): local git `show` of `aic8800_fdrv/btsdio.c`, `rwnx_msg_tx.c`, `lmac_msg.h`, and `aic8800_bsp/aic_bsp_driver.{c,h}`. Header515–518 selects mailbox under CONFIG_SDIO_BT; driver1550–1625 patches firmware config.

## Limits and next useful comparison

Observed Maix HCI TX was source-exact 254-byte PCM, ~15.875 ms median spacing; RX overwhelmingly no-data/possibly-invalid. Pristine BlueALSA v4.1.1 writes exactly its socket MTU while calculating whole S16 samples with integer division. Controller Read Buffer Size advertising255 is not necessarily socket SCO_OPTIONS255; do not assume an odd-byte encoder issue without obtaining actual socket MTU/runtime build details. The 60-byte negotiated air payload is also a different boundary.

Highest-value read-only evidence: actual PC SCO RX/TX packet lengths and setup commands, Maix SCO_OPTIONS MTU, exact running driver/firmware provenance and vendor-documented AIC SCO mailbox configuration. An explicitly authorized temporary packet-size experiment could isolate the PipeWire heuristic difference, but source inspection has not established that it fixes this device.

Skipped: device changes, upgrades, firmware edits and unsupported vendor-command guesses. Risk: inspected Sipeed source is not verified byte-for-byte against the running driver.
