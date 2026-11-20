# Investigation A — Remaining Bluetooth Kernel Logging

## 1. Root Cause & Exact Source Location

During Phase 1, suppressing `aic8800_fdrv` Wi-Fi debug logs (`aicwf_dbg_level`) reduced kernel logging CPU, but high-rate Bluetooth SDIO logs remained:

- `aic_btsdio: skb type 3`
- `aic_btsdio: hci0*` (TX path)

Inspection of the vendor driver source (`osdrv/extdrv/wireless/aic8800/aic8800_fdrv/`) identified the exact origin:

1. **Receive path (`btsdio.c:162`, `bt_sdio_recv()`)**: calls `AICBT_INFO("skb type %d", type);` on every incoming Bluetooth HCI packet over SDIO. `type == 3` (`HCI_SCODATA_PKT`) is normal synchronous SCO voice traffic (133.3 packets/sec at 8 kHz CVSD / 60-byte SCO frames).
2. **Transmit path (`btsdio.c`)**: `btsdio_send_frame()`, `btsdio_work()`, and `btsdio_tx_packet()` call `AICBT_INFO(...)` 3 times per transmitted packet (`aic_btsdio: hci0*`).
3. **Unconditional macro (`aic_btsdio.h:42`)**: `AICBT_INFO` was defined as an unconditional `printk("aic_btsdio: " fmt "\n", ## arg)` that ignored `aicwf_dbg_level`.

**Conclusion**: These messages are benign vendor debug/informational prints, not an unexpected packet type or abnormal recovery event.

## 2. Measured Message Rates by Mode

Measured via 2-second `dmesg -c` windows on the MaixCAM device:

| Operating Mode | `rx_skb3` (1.4s ring) | `tx_hci0` (1.4s ring) | Combined Rate | `klogd+syslogd` CPU |
|---|---:|---:|---:|---:|
| Video only (`DISABLE_BT_CAPTURE=1`) | 0 | 0 | 0/s | 0.0% |
| BT Mic Uplink only | 454 | 0 | ~324/s | — |
| BT Playback only (`DISABLE_BT_CAPTURE=1 --talk`) | 186 | 558 | 533.3/s (SCO is bidirectional once open) | — |
| Full-duplex (Video + Mic + Talk) | 186 | 558 | 533.3/s | 8.3% |

## 3. Minimal Kernel Module Patch

Because `AICBT_INFO` ignored `aicwf_dbg_level` at compile time, runtime sysfs/module parameters could not suppress it. We applied a minimal patch in `aic_btsdio.h` to gate `AICBT_DBG` on `aicwf_dbg_level & LOGDEBUG` and `AICBT_INFO` on `aicwf_dbg_level & LOGINFO`, while preserving `AICBT_WARN` and `AICBT_ERR` unconditionally.

- **Patch file**: [`aic8800_fdrv_gate_aicbt_info.patch`](patches/aic8800_fdrv_gate_aicbt_info.patch)
- **Build environment**: Extracted `linux_5.10`, `aic8800_fdrv`, and `aic8800_bsp` from `~/maixcam-build/licheerv-driver-source` to `/tmp/aic_build`, configured with `/proc/config.gz` (`CONFIG_LOCALVERSION="-tag-"`, `CONFIG_MODVERSIONS=n`), compiled with `riscv64-unknown-linux-musl-gcc (Xuantie-900 linux-5.10.4 musl gcc Toolchain V2.6.1 B-20220906) 10.2.0`.
- **Compatibility verification**: Confirmed `vermagic=5.10.4-tag- preempt mod_unload riscv` and 224/224 undefined symbols match `/mnt/system/ko/3rd/aic8800_fdrv.ko`.
- **Rollback**: Original module backed up on device at `/mnt/system/ko/3rd/aic8800_fdrv.ko.orig`.

```sh
# Rollback procedure on device
cp /mnt/system/ko/3rd/aic8800_fdrv.ko.orig /mnt/system/ko/3rd/aic8800_fdrv.ko
reboot
```

## 4. Before / After CPU Measurements

| Metric | Baseline (`InvB_FullDuplex`) | After Kernel Patch (`Opt1_KmodLog_FullDuplex`) | Delta |
|---|---:|---:|---:|
| `DMESG_AIC_BTSDIO_COUNT` | 533.3/s | 0 | -100% |
| `klogd + syslogd` CPU | 8.3% | 0.0% | -8.3% |
| System Busy CPU | 90.6% | 82.8% | -7.8% |
| CPU Idle Headroom | 9.4% | 17.2% | +7.8% |
| Video FPS (`cam_push`) | 27.0 fps | 28.5 fps | +1.5 fps |
| `cam_push` `nvcsw` | 477/s | 364/s | -23.7% |
