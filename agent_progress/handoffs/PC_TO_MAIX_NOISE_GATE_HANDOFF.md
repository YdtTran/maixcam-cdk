# PC → MaixCAM Audio Noise Suppression & RMS Noise Gate Handoff

## 1. Objective and Implementation Scope

This change implements real-time background noise suppression and a configurable RMS-based Noise Gate for the PC microphone before transmitting audio uplink via WebRTC to MaixCAM and playing out to Bluetooth A2DP.

- Attenuates stationary acoustic background hum and fan noise.
- Attenuates residual background noise during operator silence by exactly 35 dB (gain ~0.0178x) instead of hard zeroing.
- Preserves natural speech intelligibility and word endings without clipping or fluttering.
- Frame-independent gain smoothing (5 ms attack, 150 ms release) prevents clicks and discontinuities.
- Preserves continuous audio streaming (never stops transmitting Opus packets).
- Completely isolates microphone audio from PC speakers to prevent acoustic feedback.
- Leaves native MaixCAM A2DP Bluetooth playback, video encoding, and Wi-Fi AP completely unchanged.

## 2. Audio Pipeline Comparison

### Original Pipeline
```
PC Mic (getUserMedia, autoGainControl: true)
  → WebRTC PeerConnection (Opus 48 kHz Mono)
  → Network
  → MaixCAM WebRTC Receiver
  → libopus Decode
  → g4c4_bt_playback (BlueALSA A2DP)
  → Headset
```

### Updated Pipeline
```
PC Mic (getUserMedia with echoCancellation: true, noiseSuppression: true, autoGainControl: false)
  → Web Audio API AudioContext (48 kHz)
  → AudioWorkletNode (NoiseGateProcessor: RMS sliding window + 5-state hysteresis state machine)
  → MediaStreamAudioDestinationNode (micDestinationNode, isolated from PC speakers)
  → WebRTC PeerConnection (Opus 48 kHz Mono)
  → Network
  → MaixCAM WebRTC Receiver
  → libopus Decode
  → g4c4_bt_playback (BlueALSA A2DP)
  → Headset
```

## 3. Parameter Specifications and Defaults

| Parameter | Configured Value | Notes / Behavior |
|---|---|---|
| **Open Threshold** | `-32.0 dBFS` | CLOSED → ATTACK transition trigger on speech onset |
| **Close Threshold** | `-35.0 dBFS` | OPEN → HOLD transition trigger during pause/silence |
| **Gate Mode** | `Hysteresis` | 3 dB hysteresis gap prevents fluttering between thresholds |
| **RMS Window** | `20.0 ms` | Sliding circular buffer (960 samples @ 48 kHz) |
| **Attack Time** | `5.0 ms` | Linear gain interpolation from -35 dB to 0 dB (~240 samples) |
| **Hold Time** | `200.0 ms` | Keeps gate OPEN across short pauses between syllables/words |
| **Release Time** | `150.0 ms` | Linear gain interpolation from 0 dB down to -35 dB (~7200 samples) |
| **Closed Attenuation** | `-35.0 dB` | Multiplies signal by 0.01778x (never cuts frames or emits silence) |
| **Default Gate State** | `ENABLED` | Bypassing smoothly ramps gain to 1.0 (0 dB) without track switches |

## 4. Files Modified and Created

- `src/noise-gate-processor.js`: New pure `AudioWorkletProcessor` implementing circular buffer RMS calculation, bounded state machine, sample-rate aware interpolation, and zero-allocation per-quantum processing loop.
- `src/client.js`: Integrated `AudioContext`, `AudioWorkletNode`, safe `micDestinationNode` track routing, secure context verification, throttled telemetry handling, and UI bindings. Includes inline fallback for offline environments.
- `src/index.html`: Added compact "Audio DSP" tab in the operator vault, status badge in the header, live RMS level meter with threshold guides, open/close threshold sliders with hysteresis constraint enforcement, and reset-to-defaults controls.
- `src/maix_webrtc_http_server.hpp`: Embedded updated web assets and registered HTTP GET route `/noise-gate-processor.js`.
- `scripts/update_http_server_html.py`: Updated asset embedder to include `noise-gate-processor.js`.
- `scripts/tests/test_audio_noise_gate.py`: Automated test suite covering DSP mathematical verification and Chromium Playwright UI interaction.
- `agent_progress/standalone/evidence/audio-noise-gate/result.json`: Persistent automated test report.

## 5. Verification Commands and Outcomes

### Test Suite Execution
```sh
# Run automated DSP and browser UI regression test
build/standalone-venv/bin/python scripts/tests/test_audio_noise_gate.py

# Run existing recording persistence regression test
build/standalone-venv/bin/python scripts/tests/test_recording_persistence.py \
    --chromium /home/ydt/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome

# Run existing recording rotation regression test
build/standalone-venv/bin/python scripts/tests/test_recording_rotation.py \
    --chromium /home/ydt/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome
```

### Test Outcomes Matrix

| Test ID | Description | Result | Evidence / Measurement |
|---|---|---|---|
| **Test A** | Background Fan Noise (-38 dBFS hum) | **PASS** | Gate stays CLOSED; output is attenuated by exactly 35.0 dB to -73.0 dBFS; frames continuous |
| **Test B** | Speech Lifecycle (-20 dBFS pulse) | **PASS** | Observed states `CLOSED → ATTACK → OPEN → HOLD → RELEASE → CLOSED`; mid-speech gain = 1.0 (0 dB); attack within 5 ms |
| **Test C** | Repeated Speech (*"Alo alo — Một hai ba bốn"*) with 120 ms pauses | **PASS** | Dropout-free during pauses (200 ms hold prevents premature closing during natural speech rhythm) |
| **Test D** | Hysteresis & Bypass Toggle | **PASS** | Signal between thresholds (-34 dBFS) preserves CLOSED state; bypass restores unity gain (1.0) without audio pops |
| **UI-01** | Audio DSP Tab Navigation | **PASS** | Tab switches cleanly without impacting video stream or signaling |
| **UI-02** | Slider & Hysteresis Constraints | **PASS** | Adjusting close threshold automatically enforces `Close <= Open` clamp |
| **UI-03** | Reset to Defaults | **PASS** | Restores -32 dBFS open, -35 dBFS close, and ENABLED state |
| **UI-04** | Speaker Isolation / Feedback Safety | **PASS** | `noiseGateNode` connects only to `micDestinationNode`; zero connection to `audioContext.destination` |
| **Route-01** | Static Route `/noise-gate-processor.js` | **PASS** | Serves HTTP 200 with valid `NoiseGateProcessor` class and `registerProcessor` |
| **HW-01** | Bluetooth A2DP & Wi-Fi Coexistence | **PASS** | EDIFIER X2 Plus remains paired and connected (`Connected: yes`); Wi-Fi AP remains active on ch 36 |
| **HW-02** | Live Physical Microphone Audition | **NOT TESTED** | Automated synthetic signal suite verified; physical operator speech audition requires operator spot-check |
| **Compat-01** | Desktop Chromium / Chrome | **PASS** | Verified via Playwright headless Chromium on Linux |
| **Compat-02** | Mobile Safari / Chrome on iOS | **NOT TESTED** | Physical iOS hardware not present in headless build environment; AudioWorklet standards compliance verified |

## 6. Known Limitations

1. **Hardware-level speech audition**: While DSP simulation verifies exact attenuation and attack/hold/release characteristics, real room acoustics with fluctuating non-stationary transients (e.g. keyboard clicks or loud claps) may trigger the open threshold if they exceed -32 dBFS. The operator can raise the open threshold slider (e.g. to -28 or -24 dBFS) to accommodate louder environments.
2. **Old Browser Fallback**: If an older browser lacks `AudioWorklet` support, the client logs a warning and routes the raw `getUserMedia()` track directly to `audioTransceiver`, allowing two-way communication without noise gating.

## 7. Rollback Instructions

If rollback is required, run the device rollback script generated during deployment:

```sh
ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-040215-f4dfc2fc/rollback.sh'
```

Or roll back to the previous RC-03 release:
```sh
ssh root@10.172.16.1 'sh /root/maixcam-standalone-20261101-034054-810730ff/rollback.sh'
```

## 8. Recommended Next Steps

1. Operator spot-check: Access `https://maixcam.home.arpa:8000/` or `https://192.168.4.1:8000/`, open the new **Audio DSP** tab, click **Talk**, and speak naturally while listening through the EDIFIER X2 Plus headset.
2. Confirm the live RMS level bar and gate status pill change dynamically (`CLOSED` during pauses, `OPEN` during speech).
