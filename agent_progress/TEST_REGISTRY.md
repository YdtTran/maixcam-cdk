# Standalone test registry

| Test | Outcome | Evidence |
|---|---|---|
| Pre-change Wi-Fi + USB SSH | PASS | standalone/AUDIT.md; audit-initial.txt |
| Original binary identity matches deployed baseline | PASS | final-health.txt; original git artifact SHA in audit/recovery handoff |
| Native cross-build/link | PASS | build-preserve-toggle.txt |
| Target ABI and transfer checksum | PASS | deploy-preserve-toggle.txt; final-health.txt |
| Target BusyBox gzip tar attempt | FAIL before mutations; fixed with plain tar | deploy-first.txt; SIGNALING_HANDOFF.md |
| Early WSS callback/lifetime | FAIL; fixed and retested | media-test-run.txt; media-test-run2.txt; signaling.json |
| Native trusted HTTPS/WSS/asset health | PASS | deploy-preserve-toggle.txt |
| AP 5 GHz channel 36 + actual client association/DHCP | PASS | ap-client-status.txt |
| 2.4 GHz channel 1 fallback after primary failure | PASS; 5 GHz restored | fallback-test.txt; final-health.txt |
| Foreign HTTP Origin rejection | PASS | test_device.py reached media stage after origin assertion |
| WSS host-only ICE, three reconnects, one owner, invalid token | PASS | signaling.json |
| Idle cleanup / Talk mute after disconnect | Initial FAIL, corrected PASS at ~30 seconds | signaling-run.txt; signaling-final-run.txt; signaling.json |
| VM-to-USB UDP probe / automated media | FAIL / route blocked; not treated as AP media result | udp-probe.txt; media-test-run3.txt |
| Idempotent service start, AP/process recovery | PASS | recovery-test.txt |
| Targeted headset reconnection | PASS, ≤4 seconds; media/AP remained running | bluetooth-recovery.txt |
| Software reboot + standalone AP/app/BT startup | PASS, observed READY after 65.18 seconds | reboot.json |
| Original Talk toggle and inline rotation/style preservation | PASS source/served-client checks; new Hold mode removed | ui-preservation.txt |
| AP browser video + bidirectional audio | PASS, operator-confirmed | operator-confirmation.md |
| Browser trusted CA / no certificate warning | PASS, operator-confirmed | operator-confirmation.md |
| Baseline/post-boot/final idle resources | PASS measured; no controlled workload comparison | baseline-cpu.json; postboot-cpu.json; final-cpu.json; raw reports |
| Final operator-session cumulative counters | Activity observed, including 294 playback queue overflows; not a controlled stability PASS | final-operator-counters.json |
| Full-load sustained resource/FPS/loss/jitter/dropout/sync test | PENDING; host VM cannot directly exercise AP UDP | HANDOFF.md |
| Distinct manual rotation check, client Wi-Fi interruption, physical power cycle | PENDING | HANDOFF.md |
| HFP active audio path | NOT VERIFIED in original/runtime; only A2DP source found | AUDIT.md |

Paths in Evidence column are under `agent_progress/standalone/evidence` unless a handoff is named. Historical failed/discarded tests are retained without being counted as final successes. No test result is inferred from a build/process exit alone. Persistent target artifacts live under `/root/maixcam-tests/standalone-web`.
