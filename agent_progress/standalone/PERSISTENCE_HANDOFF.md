# RC-03: Keep recordings after F5

Objective: retain completed recordings, preview and downloads across browser refresh/restart.

Implementation: same-origin browser IndexedDB database maixcam-recordings, version 1, recordings store keyed by collision-resistant dated filename. Store the encoded Blob, creation time, format, video/audio kind and duration. Report Saved only on transaction completion. Restore saved objects into new Blob URLs/rows after loading. Saves retain the existing rotated canvas media stream and audio tracks; no native camera/audio/Bluetooth or signaling logic changes. Camera flash/RAM is not used for clip storage.

Delete waits for an in-flight save, commits the persistent delete, then clears preview/row and revokes the URL. A failed delete leaves the saved object and row intact. Quota/write failure keeps an accessible preview/download with Not saved messaging. Before-unload protects an active recording or unfinished/failed save/deletion; completed Saved clips allow normal F5. Request persistent browser storage once on first recording, without assuming permission was granted. Storage remains scoped to the same browser profile and URL origin, with ordinary browser site-data/private-mode limits.

Operator confirmed there were no old page-only clips that required preserving before deployment. This implementation does not retroactively recover discarded Blob URLs from the previous version.

Files: src/index.html, generated src/maix_webrtc_http_server.hpp, scripts/tests/test_recording_persistence.py and the existing real-media regression test (now loaded on a real origin to permit IndexedDB, with async delete awaited), operational docs and indexes.

Verification: actual Chromium with a disk profile recorded video/audio, reloaded, checked Saved/list restoration and a decoded 480x640 portrait video preview, downloaded the same bytes (SHA-256 equality), closed/reopened the browser process and restored both video/audio. Persistent delete survived another refresh. Injected QuotaExceededError retained the unsaved downloadable clip and warning; injected delete failure retained the saved audio and row. Existing four-angle recording/preview, mid-record rotation, codec fallback and shared-track cleanup regression passed.

Evidence under agent_progress/standalone/evidence: recording-persistence-test.txt, recording-persistence/result.json, preview-after-refresh.png and restored-video.webm; recording-persistence-regression.txt; build-recording-persistence.txt; deploy-recording-persistence.txt (interrupted); deploy-recording-persistence-retry.txt (active release `/root/maixcam-standalone-20261101-034054-810730ff`). Test profile files were created under the persistent evidence directory, never /tmp; only isolated synthetic test data was cleaned up.

Client steps: refresh to load this update; record, stop, wait until Saved; F5 and use Preview/Download on the restored row. Do not clear site data or change URL/profile if expecting the same local library. Browser error/quota paths are tested by injection, not destructive disk exhaustion. Physical camera recording remains an operator spot-check; the native media pipeline is unchanged.
