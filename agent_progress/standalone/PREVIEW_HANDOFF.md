# RC-02: Real recording video preview

Objective: retain the preview only if it actually displays video, including the recorded orientation.

Implementation: an inline video element alongside the audio-only player; playRecord selects the correct player and pauses/clears the other. Local recordings resolve directly to their Blob URLs through a map, not a nonexistent /recordings URL. New rows have a Preview action, with milliseconds in generated names to avoid collisions during quick recordings. Delete stops the selected player, resets the panel and revokes the local Blob URL. Missing/unsupported files show a preview error; the old synthesized-tone fallback is removed. Action buttons wrap to stay accessible in the narrow sidebar. Live camera, Talk, codecs, filtering and Bluetooth code are unchanged.

Files: src/index.html, generated src/maix_webrtc_http_server.hpp, scripts/tests/test_recording_rotation.py and operational documentation. No new server/runtime/storage service.

Tests: real Chromium recordings were encoded/decoded at all four angles; the actual inline video element was sampled and its colors/dimensions matched the saved/displayed clip, including 90-degree portrait. Preview visibility and sidebar button bounds were checked; a screenshot is retained as recording-rotation/preview-video-90.png. Audio-only mode selected the audio player; replay from another row switched back to video; deletion stopped/reset/revoked the active preview; missing files reported failure without synthetic sound. Existing recording rotation, mid-record changes, codec fallback and shared-track cleanup tests still pass.

Evidence: recording-preview-test.txt, recording-rotation/result.json, preview-video-90.png, build-recording-preview-final.txt and deploy-recording-preview-final.txt under agent_progress/standalone/evidence. Build/deployment identity and trusted served-asset verification are recorded with this milestone. Synthetic media was used in browser tests; physical camera/audio output was not newly claimed manually verified.

Usage: refresh the UI, record a clip, stop to see its video preview; use Preview to replay. Audio-only clips keep audio controls. Local recordings last for the current page session: download before refresh/close. Browser playback still depends on supported codecs and autoplay permissions (press Play if required).

Deployed release: `/root/maixcam-standalone-20261101-033130-22603900`. Verified trusted HTTPS serves the exact tested HTML; native service health gate checks HTTPS/WSS and AP. SHA-256: `2260390075a57488ae7272792901b3330f5eddf625c858fcd087a88cf1fed553`. Rollback to previous release: `ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-033130-22603900/rollback.sh'`.
