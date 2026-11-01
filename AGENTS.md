# Repository Guidelines

## Project Structure & Module Organization

This project runs a native C++ WebRTC camera and duplex audio service on MaixCAM (RISC-V musl Linux).

- `src/maix_webrtc_maixcam.cpp`: camera capture, encoding, and microphone integration.
- `src/maix_webrtc_server.hpp` and `src/maix_webrtc_http_server.hpp`: WebRTC, HTTP, signaling, and embedded web assets.
- `src/g4c4_*.hpp`: Bluetooth playback, Opus decoding, and resampling.
- `src/index.html`: operator cockpit UI, including inline styles.
- `scripts/`: cross-compilation, asset embedding, deployment, and integration testing.
- `target_services/`: device service control and boot scripts.
- `build/`: native archives, metadata, executable, and runtime shared libraries.

## Build, Test, and Development Commands

Run commands from the repository root:

- `python3 scripts/update_http_server_html.py`: regenerate embedded HTML and client JavaScript. Run after UI changes; update the script’s `client_js` when changing generated signaling code.
- `python3 scripts/build.py`: cross-compile and link `build/stream_webrtc-m2`; diagnostics go to `build/compile.log` and `build/link.log`.
- `bash scripts/deploy.sh root@<device-ip>`: transfer runtime files, verify the executable checksum, and restart the device service. This stops the active stream.
- `python3 scripts/test_webrtc_aiortc.py`: exercise signaling and audio playback against the default device at `192.168.1.8`.

Building requires `build/libvision.a`, `build/build-metadata.json`, its referenced toolchain, and the expected `~/maixcam-build/MaixCDK-opus-g4a` SDK tree. The service runs on hardware; open `http://<device-ip>:8000/` for the UI.

## Device Access and Connection Recovery

- Prefer the device's Wi-Fi connection for normal operation.
- If Wi-Fi disconnects or the device becomes unreachable over Wi-Fi, use the attached USB network connection: `ssh root@10.172.16.1`. The SSH password is `root`.
- Use this USB connection for diagnosis and recovery when Wi-Fi is unavailable; device commands and deployment can target `root@10.172.16.1`.
- If the USB connection also fails, report the connection failure to the user and ask them to hard reset the device. Do not assume the device has recovered or continue device-dependent steps until connectivity is restored.

## Coding Style & Naming Conventions

Match surrounding code: four-space indentation in C++, Python, and frontend code; preserve existing shell conventions. Use snake_case for Python functions and existing C++ helpers, PascalCase for classes, and uppercase constants. Preserve `maix_webrtc_*` and `g4c4_*` module names. No formatter or linter configuration is provided.

## Testing Guidelines

The integration client requires `aiohttp`, `aiortc`, and PyAV (`av`), a running device, and Bluetooth audio hardware. It sends a sine tone and prints audio statistics; inspect those statistics and confirm audible playback. For another address, call `run_test(target_ip=...)` through Python. No unit-test framework or coverage threshold is configured. Manually verify video, Talk/Listen, and affected UI controls; confirm Bluetooth disconnection leaves video and microphone streaming operational.

## Commit & Pull Request Guidelines

The existing history uses `feat: ...`; follow that prefix style with concise imperative summaries, using `fix:` or `docs:` where appropriate. PRs should describe behavior changes, hardware/SDK assumptions, and validation results. Link relevant issues and include screenshots for UI changes. Include regenerated headers with asset changes; exclude logs and temporary recordings.
