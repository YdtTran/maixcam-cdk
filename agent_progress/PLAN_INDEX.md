# Standalone implementation plan index

Historical PLAN-00..15 entries are preserved verbatim in [LEGACY_PLAN_INDEX.md](LEGACY_PLAN_INDEX.md), copied from the original integration repository. No original entries or baseline artifacts were overwritten.

| Plan | Objective | Status | Evidence / handoff |
|---|---|---|---|
| SA-A | Repository, hardware, ABI and USB recovery audit | PASS | [Audit](standalone/AUDIT.md) |
| SA-B | Camera-owned AP, DHCP/DNS, safe fallback and rollback | PASS | [Recovery](standalone/RECOVERY_HANDOFF.md) |
| SA-C | Fully local embedded frontend | PASS | [Web/signaling](standalone/SIGNALING_HANDOFF.md) |
| SA-D | Local trusted WSS, host ICE, one owner and reconnect | PASS | [Web/signaling](standalone/SIGNALING_HANDOFF.md) |
| SA-E | Native HTTPS and client trust provisioning | PASS: private CA trusted by operator browser, no warnings | [Final handoff](standalone/HANDOFF.md) |
| SA-F | Preserve media and original Talk/rotation controls | PARTIAL: operator video/audio PASS; detailed media/rotation tests pending | [Final handoff](standalone/HANDOFF.md) |
| SA-G | AP/Bluetooth coexistence | PARTIAL: reconnect PASS; controlled full-load soak pending | [Recovery](standalone/RECOVERY_HANDOFF.md) |
| SA-H | Boot and service/process recovery | PASS: software reboot and daemon recovery | [Recovery](standalone/RECOVERY_HANDOFF.md) |
| SA-I | Resource/performance measurements | PARTIAL: idle/baseline measured; active soak pending | [Final handoff](standalone/HANDOFF.md) |
| SA-J | Reproducible deployment, rollback and documentation | PASS | [Operations](../docs/STANDALONE.md), [Final handoff](standalone/HANDOFF.md) |

Current deployed release (RC-04): `/root/maixcam-standalone-20261101-040215-f4dfc2fc`.

Operator overrides: open AP, no Wi-Fi password; original click-to-toggle Talk, no new Hold mode. Operator confirmed video and bidirectional audio. Overall implementation is deployed; remaining acceptance limits are explicitly retained in the final handoff.

| Follow-up | Objective | Status | Handoff |
|---|---|---|---|
| RC-01 | Record the displayed camera orientation, including changes while recording | PASS: real-browser encode/decode, target health and served assets verified | [Recording handoff](standalone/RECORDING_HANDOFF.md) |
| RC-02 | Keep real video/audio recording preview, replay and deletion | PASS: browser media/preview regression and trusted served assets | [Preview handoff](standalone/PREVIEW_HANDOFF.md) |
| RC-03 | Retain completed recordings across F5/browser restart with preview/download | Real profile persistence/failure tests PASS; deployed `/root/maixcam-standalone-20261101-034054-810730ff` | [Persistence handoff](standalone/PERSISTENCE_HANDOFF.md) |
| RC-04 | Real-time PC mic background noise suppression & hysteresis RMS noise gate | Automated DSP tests PASS, Chromium UI & route tests PASS; deployed `/root/maixcam-standalone-20261101-040215-f4dfc2fc` | [Noise Gate handoff](handoffs/PC_TO_MAIX_NOISE_GATE_HANDOFF.md) |
