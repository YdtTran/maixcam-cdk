# Record the displayed rotation

Objective: save the camera image exactly as composed in the cockpit, including manual rotation, instead of recording the unrotated remote track.

Source changes: `src/index.html`, regenerated `src/maix_webrtc_http_server.hpp`; browser regression at `scripts/tests/test_recording_rotation.py`. Existing camera rendering/rotation math, signaling, Talk toggle, native camera/encoder/microphone/filter and Bluetooth backend are unchanged.

Implementation: copy each rendered display frame to a recording canvas during the existing painter callback; capture that canvas at up to 30 FPS and combine its video track with the existing remote audio tracks. Initial recording dimensions follow the displayed orientation. Mid-record rotations fit into a fixed recording size with black margins, avoiding encoder dimension changes or additional clipping. Codec fallback retains this composed stream. Stop/error paths release only owned canvas tracks. Audio-only mode remains audio-only. A second recording cannot start while the previous stop event is finalizing its chunks.

Validation: actual Chromium MediaRecorder output was decoded and compared with the displayed quadrant pixels at 0/90/180/270; portrait recordings were 480x640, landscape 640x480. A mid-record rotation was checked on the final encoded frame using FFmpeg, including black margins. FFprobe found H.264 video and Opus audio in a 90-degree recording. Tests confirmed codec fallback keeps the canvas, original audio/video tracks remain live after stop, owned tracks end, and audio-only recording has zero video tracks. Synthetic media was used; physical camera recording remains an operator spot-check.

Commands: asset regeneration, native build, real Chromium regression, FFprobe, `git diff --check`, standalone deployment via USB, and trusted HTTPS comparison of the served assets. See persistent evidence `recording-rotation-test.txt`, `recording-rotation/result.json`, `recording-rotation/probe-90.json`, `build-recording-rotation.txt`, `deploy-recording-rotation.txt`.

Build SHA-256: `5fccbdece284ba63d45f550b75604c98056a1894063c165b082941b9b9078fd7`.

Deployment PASS: `/root/maixcam-standalone-20261101-032316-5fccbdec`, READY with trusted HTTPS/WSS; fetched embedded HTML exactly matches src/index.html. Rollback: `ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-032316-5fccbdec/rollback.sh'`. Evidence: `deploy-recording-rotation.txt`, `recording-deployed-assets.json`; every release's rollback restores its predecessor. The original standalone acceptance records remain historical. No new commit was made as part of this feature request.
