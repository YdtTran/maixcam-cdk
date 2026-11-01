# Native web/signaling milestone (C–E)

Objective: serve the complete existing UI and signaling on MaixCAM with offline assets and trusted TLS, preserving the media implementation.

Status: **PASS for native HTTPS/WSS protocol tests and operator-confirmed private-CA browser trust/microphone/video/audio operation. Quantitative media/soak acceptance remains separate.**

Inspected: original embedded HTML/JS, generator, SDK httplib/rtc signaling and TLS configuration, native builder and ICE setup. Changed: `src/client.js` (canonical working client), `src/index.html`, generated `src/maix_webrtc_http_server.hpp`, local `src/maix_webrtc_signaling_server.hpp`, minimal native wrappers, and asset generator.

Implementation: native httplib SSLServer on 8000, existing rtc WebSocketServer with TLS on 8001. Same leaf/key, same host, token in WSS URL delivered by same-origin client.js. Foreign HTTP Origin rejected. No permissive CORS. One admitted session; pending TLS sockets are bounded and retained until onOpen; close/timeout resets the peer and mutes Talk. Client heartbeats/reloads recover signaling. Host-only ICE in standalone mode; no public STUN/TURN. DTLS-SRTP and encoder/capture/filter/backend code are unchanged. External fonts and adapter CDN are removed; all remaining assets are embedded.

Commands: `python3 scripts/update_http_server_html.py`, `python3 scripts/build.py`, `node --check src/client.js`, `python3 -m py_compile ...`, `python3 scripts/standalone/deploy.py --skip-build`, and `build/standalone-venv/bin/python scripts/standalone/test_signaling.py`.

Evidence/results: `evidence/build-preserve-toggle.txt`, `evidence/deploy-preserve-toggle.txt`, `evidence/signaling.json`, `evidence/signaling-final-run.txt`, `evidence/ui-preservation.txt`. Verified trusted WSS, three reconnects, second-client busy rejection without stealing the owner, wrong-token rejection, host-only candidate offers, ~30-second idle cleanup and muted Talk after disconnect. The initial disconnect test found the legacy decoder defaults to Talk ON when recreated; standalone initialization now explicitly mutes the new RX worker without changing the decoder/backend.

Failures retained: first deployment stopped before mutation because this BusyBox tar lacks `-z`; archive handling was corrected. Early WSS tests failed because the URL was inspected before onOpen and public sockets were not retained during handshake; both defects were fixed and retested. These failed releases were not accepted as final healthy deployments. See deploy/media/signaling run logs.

Operator correction: the initial Hold mode addition was removed entirely after explicit feedback. The final Talk button uses the original click toggle and the same notify/start/stop functions. Historical `hold-talk.txt` only records a discarded implementation test; it is not final product behavior or evidence. The original inline UI scripts/styles and g4c4 backend sources are preserved.

Private CA: CA key remains only on the provisioning host in ignored root-protected secrets; leaf/key/CA certificate go to camera. Client trust installation is independent of AP association. Provisioning supports a configured domain; rotation reuses the CA and redeploys a fresh leaf. Public domain/DNS-01 credentials were not supplied.

Rollback: latest release `rollback.sh` restores the preceding configuration. The first successful standalone release's rollback returns to the original router-based deployment. See docs/STANDALONE.md.

Operator subsequently confirmed video, bidirectional audio, trusted CA and no certificate warnings. Next: distinct rotation/control checks and controlled sustained full-load AP media measurements.
