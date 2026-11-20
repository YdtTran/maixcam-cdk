# Investigation D — Wi-Fi & Bluetooth Coexistence

## 1. Hardware Architecture & Coexistence Context

The MaixCAM uses an **AIC8800** combo Wi-Fi + Bluetooth chip connected over a shared **SDIO (`mmc1`, IRQ 27)** bus. Bluetooth HFP/SCO runs in the 2.4 GHz ISM band, while the default standalone AP (`hostapd`) operates in the 5 GHz band (channel 36, 5180 MHz, `192.168.4.1/24`).

## 2. Measured Transport Modes (Simultaneous Full-Duplex Video + BT SCO)

| Metric | USB RNDIS (`10.172.16.1`) | Mode B: 5 GHz STA (`5240 MHz`, `-64 dBm`) | Mode C: 2.4 GHz STA (`2462 MHz`, `-64 dBm`) |
|---|---:|---:|---:|
| System Busy CPU | 81.5% | 80.3% | 81.8% |
| CPU Idle Headroom | 18.5% | 19.7% | 18.2% |
| `IRQ 27 (mmc1)` rate | 1483.8/s | 1474.7/s | 1484.5/s |
| Video FPS (device sent) | 28.8 fps | 28.9 fps | 29.1 fps |
| Video FPS (browser disp) | 29.16 fps | 29.16 fps | 29.28 fps |
| Video `pkts_lost` | 0 | 0 | 0 |
| Video `jitter_ms` | 4.00 ms | 5.00 ms | 6.00 ms |
| Video `jb_ms` | 31.84 ms | 49.60 ms | 56.21 ms |
| Audio Uplink `pkt/s` | 50.0/s | 50.0/s | 50.0/s |
| Audio `pkts_lost` | 0 | 0 | 0 |
| Audio `jitter_ms` | 3.00 ms | 4.00 ms | 3.00 ms |
| Audio `jb_ms` | 35.73 ms | 59.01 ms | 56.55 ms |
| Audio `concealed` | 0 | 0 | 0 |
| Downlink `underruns` | 0 | 0 | 0 |

### Note on Mode A (Standalone 5 GHz AP, Channel 36 / 5180 MHz)

Standalone 5 GHz AP mode (`/etc/maixcam-standalone/hostapd.conf`: `hw_mode=a`, `channel=36`, `ieee80211n=1`, `wmm_enabled=1`, `192.168.4.1/24`) was verified active and reachable (`ping rtt min=2.51ms`, `wlan0` UP). A full 32-second headless browser run could not be held simultaneously on the test laptop's single Wi-Fi NIC without dropping the host's internet connection. Based on the 5 GHz STA measurement (which shares the exact same AIC8800 5 GHz RF + SDIO path), 5 GHz operation avoids 2.4 GHz RF contention with Bluetooth.

## 3. Coexistence Findings

1. **5 GHz vs. 2.4 GHz**: Both 5 GHz STA (`5240 MHz`) and 2.4 GHz STA (`2462 MHz`) sustained **0% packet loss** and **0 concealed audio samples** over 32-second full-duplex runs at `-64 dBm` RSSI.
2. **Jitter Buffer Impact**: 2.4 GHz STA showed slightly higher video jitter (`6.00 ms` vs. `5.00 ms` on 5 GHz and `4.00 ms` on USB) and higher video jitter-buffer delay (`56.21 ms` vs. `49.60 ms` on 5 GHz and `31.84 ms` on USB).
3. **Operational Note on BlueALSA + `hci0` Restarts**: Stopping `bluealsa` mid-SCO stream during rapid service restarts can trigger `Bluetooth: hci0: command 0x0406 tx timeout` (`Disconnect`) and `aicwf_sdio` `cmd timed-out` / `wlan error reset flow`. Never reset `hci0` (`hciconfig hci0 down/up`) while Wi-Fi is active.

## 4. Restoration

Standalone 5 GHz AP mode (`MaixCAM`, channel 36, `192.168.4.1/24`) and `/etc/init.d/S99webrtc_autostart` were restored immediately after testing.
