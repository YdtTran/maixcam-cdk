# MaixCAM Native WebRTC + Bluetooth Cockpit

Hệ thống trạm điều khiển (Operator Cockpit) truyền phát Video WebRTC thời gian thực, thu âm Micro trường tại chỗ, và đàm thoại hai chiều phát trực tiếp qua tai nghe Bluetooth A2DP trên nền tảng MaixCAM (Sophgo CV182x / RISC-V 64-bit musl Linux).

---

## 1. Cấu trúc thư mục dự án

```text
maixcam-webrtc-cockpit/
├── src/                                  # Mã nguồn C++ & Frontend Web
│   ├── maix_webrtc_maixcam.cpp           # Camera VPSS, WebRTC pipeline & Mic capture
│   ├── maix_webrtc_server.hpp            # Libdatachannel native wrapper
│   ├── maix_webrtc_http_server.hpp       # HTTP Server nhúng phục vụ Web & Signaling
│   ├── g4c4_bt_playback.hpp              # ALSA/BlueALSA A2DP worker thread
│   ├── g4c4_opus_rx.hpp                  # Opus packet decoder worker (WebRTC -> PCM)
│   ├── g4c4_resampler.hpp                # Software Resampler (8 kHz -> 48 kHz stereo)
│   └── index.html                        # Giao diện Operator Console (100vh Dark Cockpit)
├── build/                                # Binary & Thư viện liên kết
│   ├── stream_webrtc-m2                  # Binary thực thi chính (RISC-V musl)
│   ├── libvision-m2.a                    # Archive thư viện vision
│   ├── libvision.a                       # Archive nền tảng
│   ├── build-metadata.json               # Cờ biên dịch & liên kết toolchain
│   └── dl_lib/                           # Toàn bộ thư viện chia sẻ (.so) chạy trên MaixCAM
├── scripts/                              # Công cụ Build, Deploy & Test
│   ├── build.py                          # Biên dịch chéo C++ ra binary RISC-V musl
│   ├── deploy.sh                         # Triển khai 1-chạm sang MaixCAM qua SSH
│   ├── update_http_server_html.py        # Tự động nhúng index.html vào mã nguồn C++
│   └── test_webrtc_aiortc.py             # Script kiểm thử tự động duplex audio/video
├── target_services/                      # Service chạy trên MaixCAM (/etc/init.d/)
│   ├── webrtc_service.sh                 # Bộ điều khiển dịch vụ: start | stop | restart | status
│   ├── S42bt_manager                     # Daemon tự động kết nối lại tai nghe Bluetooth
│   └── S99webrtc_autostart               # Script tự động bật WebRTC khi cấp nguồn
└── README.md                             # Tài liệu hướng dẫn sử dụng
```

---

## 2. Các tính năng cốt lõi

1. **Video Streaming:**
   - Cảm biến GC4653 $\to$ MMF VPSS $\to$ Mã hóa phần cứng H.264 $\to$ WebRTC Track ($640 \times 480$ @ 30 FPS).
   - Chuẩn hóa góc nhìn thẳng đứng ở $0^\circ$ (baseline crop đều trên/dưới, lấp đầy tỉ lệ 4:3, loại bỏ hoàn toàn viền đen).
   - Hỗ trợ xoay hình ảnh mượt mà qua các góc $0^\circ, 90^\circ, 180^\circ, 270^\circ$ bằng 2D Canvas Compositor (chống hiện tượng màn hình đen DirectComposition trên Chromium).
2. **Audio Uplink (Micro MaixCAM $\to$ Trình duyệt):**
   - Card 0 ADC (`cv182xaadc`) $\to$ TinyALSA (`pcmC0D0c`) $\to$ Opus 48 kHz Mono $\to$ WebRTC Audio Track.
3. **Audio Downlink (Trình duyệt $\to$ Tai nghe Bluetooth):**
   - Micro trình duyệt $\to$ WebRTC Opus RTP $\to$ `opus_decode()` (8 kHz PCM) $\to$ Resampler $\to$ ALSA BlueALSA A2DP $\to$ Tai nghe Bluetooth (ví dụ: `EDIFIER X2 Plus` tại `6C:16:29:7B:00:D4`).
   - Độc lập hoàn toàn với luồng Video: ngắt kết nối Bluetooth không bao giờ làm gián đoạn Video hoặc Mic.
4. **Operator Cockpit:**
   - Call dock điều khiển Talk / Listen / Rotate / Snapshot / Session Recording / Fullscreen.
   - Recordings Vault: quản lý, phát lại và xóa các tệp âm thanh ghi âm tại hiện trường.

---

## 3. Hướng dẫn phát triển & Thay đổi giao diện

Nếu bạn sửa giao diện trong [`src/index.html`](src/index.html):

```bash
# 1. Đồng bộ file index.html vào header C++ maix_webrtc_http_server.hpp
python3 scripts/update_http_server_html.py

# 2. Biên dịch lại binary
python3 scripts/build.py

# 3. Đẩy lên thiết bị và khởi động lại dịch vụ
bash scripts/deploy.sh root@192.168.1.8
```

---

## 4. Hướng dẫn Deploy lên một thiết bị MaixCAM mới

### Cách 1: Sử dụng script Deploy từ máy tính
```bash
bash scripts/deploy.sh root@<IP_MAIXCAM>
```

### Cách 2: Copy thủ công bộ Runtime lên MaixCAM
Chỉ cần copy thư mục `build/` và các service:
1. Copy `build/stream_webrtc-m2` và `build/dl_lib/` vào `/root/webrtc-runtime/` trên MaixCAM.
2. Copy `target_services/webrtc_service.sh` vào `/root/webrtc_service.sh` (`chmod +x`).
3. Khởi chạy:
   ```bash
   /root/webrtc_service.sh start
   ```

---

## 5. Quản lý dịch vụ trên MaixCAM

Truy cập SSH vào MaixCAM:
```bash
# Kiểm tra trạng thái và thông số kết nối
/root/webrtc_service.sh status

# Khởi động lại dịch vụ
/root/webrtc_service.sh restart

# Dừng dịch vụ
/root/webrtc_service.sh stop
```

Truy cập giao diện Web: `http://<IP_MAIXCAM>:8000/`  
Signaling WebSocket: `ws://<IP_MAIXCAM>:8001/`
