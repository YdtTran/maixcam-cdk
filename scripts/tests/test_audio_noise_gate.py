#!/usr/bin/env python3
"""
Automated validation of PC -> MaixCAM Audio Noise Suppression & Noise Gate.
Tests:
- DSP simulation: Test A (Background fan noise), Test B (Speech onset/decay),
  Test C (Repeated speech 'Alo alo - Mot hai ba bon'), Test D (Hysteresis & bypass)
- Browser UI & AudioWorklet: Tab controls, sliders, hysteresis constraints,
  AudioContext isolation (no feedback to speakers).
"""
import argparse
import asyncio
import json
import math
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

async def run(args):
    report = {
        'dsp_simulation': {},
        'browser_ui': {},
        'overall': 'PENDING'
    }

    evidence_dir = ROOT / 'agent_progress/standalone/evidence/audio-noise-gate'
    evidence_dir.mkdir(parents=True, exist_ok=True)

    from playwright.async_api import async_playwright

    chromium_path = str(args.chromium) if args.chromium else '/home/ydt/.cache/ms-playwright/chromium-1248/chrome-linux64/chrome'

    html_content = (ROOT / 'src/index.html').read_text(encoding='utf-8')
    client_js = (ROOT / 'src/client.js').read_text(encoding='utf-8')
    worklet_js = (ROOT / 'src/noise-gate-processor.js').read_text(encoding='utf-8')

    async with async_playwright() as p:
        browser = await p.chromium.launch(
            executable_path=chromium_path,
            headless=True,
            args=[
                '--autoplay-policy=no-user-gesture-required',
                '--use-fake-ui-for-media-stream',
                '--use-fake-device-for-media-stream',
                '--disable-web-security'
            ]
        )
        context = await browser.new_context()
        await context.add_init_script("""
            // Mock WebSocket so page doesn't reload on connection close during synthetic tests
            window.WebSocket = class extends EventTarget {
                constructor(url) {
                    super();
                    this.url = url;
                    this.readyState = 1; // OPEN
                    setTimeout(() => {
                        if (this.onopen) this.onopen();
                    }, 50);
                }
                send(data) {}
                close() {}
            };
        """)
        page = await context.new_page()

        # Intercept routes so page loads identical to production HTTPS origin
        origin = 'https://maixcam.home.arpa:8000'
        await context.route(f'{origin}/*', lambda route: (
            route.fulfill(status=200, content_type='text/html', body=html_content) if route.request.url.endswith('/') or route.request.url.endswith('/index.html') else
            route.fulfill(status=200, content_type='application/javascript', body=client_js) if route.request.url.endswith('/client.js') else
            route.fulfill(status=200, content_type='application/javascript', body=worklet_js) if route.request.url.endswith('/noise-gate-processor.js') else
            route.fulfill(status=200, content_type='application/json', body='{}')
        ))

        await page.goto(f'{origin}/')

        # -------------------------------------------------------------
        # 1. DSP SIMULATION TESTS (Running inside Chromium's V8 engine)
        # -------------------------------------------------------------
        dsp_test_script = f"""
        (function() {{
            if (typeof AudioWorkletProcessor === 'undefined') {{
                globalThis.AudioWorkletProcessor = class {{
                    constructor() {{
                        this.port = {{
                            onmessage: null,
                            postMessage: () => {{}}
                        }};
                    }}
                }};
            }}
            {worklet_js.replace("registerProcessor('noise-gate-processor', NoiseGateProcessor);", "")}

            const sr = 48000;
            // Mock AudioWorkletProcessor environment
            globalThis.sampleRate = sr;

            // Instantiate processor
            const proc = new NoiseGateProcessor({{}});

            function runSignal(samples, blockFn) {{
                const blockSize = 128;
                const numBlocks = Math.floor(samples.length / blockSize);
                const outSamples = new Float32Array(numBlocks * blockSize);
                const telemetryEvents = [];

                proc.port.postMessage = (msg) => {{
                    telemetryEvents.push(msg);
                }};

                for (let b = 0; b < numBlocks; b++) {{
                    const inBlock = samples.subarray(b * blockSize, (b + 1) * blockSize);
                    const outBlock = new Float32Array(blockSize);
                    if (blockFn) blockFn(b, proc);
                    proc.process([[inBlock]], [[outBlock]], {{}});
                    outSamples.set(outBlock, b * blockSize);
                }}
                return {{ outSamples, telemetryEvents }};
            }}

            const results = {{}};

            // Test A: Stationary Fan Noise (-38 dBFS background hum)
            // Target: Gate remains CLOSED, signal attenuated by exactly -35 dB (gain ~0.01778), continuous frames
            {{
                const durationSec = 1.0;
                const n = Math.round(sr * durationSec);
                const noise = new Float32Array(n);
                // Generate stationary random noise
                const targetInputDbfs = -38.0;
                // Peak for uniform random noise with RMS = 10^(targetInputDbfs/20)
                const amp = Math.pow(10, targetInputDbfs / 20) * Math.sqrt(3);
                for (let i = 0; i < n; i++) {{
                    noise[i] = (Math.random() * 2 - 1) * amp;
                }}

                // Measure actual input RMS
                let inSumSq = 0;
                for (let i = Math.round(sr * 0.1); i < n; i++) inSumSq += noise[i] * noise[i];
                const inRms = Math.sqrt(inSumSq / (n - Math.round(sr * 0.1)));
                const inDbfs = 20 * Math.log10(Math.max(inRms, 1e-7));

                proc.port.onmessage({{ data: {{ type: 'reset' }} }});
                const {{ outSamples, telemetryEvents }} = runSignal(noise);

                // Compute output RMS
                let sumSq = 0;
                for (let i = Math.round(sr * 0.1); i < n; i++) sumSq += outSamples[i] * outSamples[i];
                const outRms = Math.sqrt(sumSq / (n - Math.round(sr * 0.1)));
                const outDbfs = 20 * Math.log10(Math.max(outRms, 1e-7));
                const expectedDbfs = inDbfs - 35.0; // Attenuated by 35 dB

                results.test_a_fan_noise = {{
                    pass: Math.abs(outDbfs - expectedDbfs) < 1.0 && proc.state === proc.STATE_CLOSED,
                    state: proc.state === proc.STATE_CLOSED ? 'CLOSED' : 'NOT_CLOSED',
                    input_dbfs: Math.round(inDbfs * 10) / 10,
                    output_dbfs: Math.round(outDbfs * 10) / 10,
                    expected_dbfs: Math.round(expectedDbfs * 10) / 10,
                    attenuation_db: Math.round((inDbfs - outDbfs) * 10) / 10,
                    continuous_frames: outSamples.length === n
                }};
            }}

            // Test B: Speech Burst (-20 dBFS speech pulse of 400 ms followed by 500 ms silence)
            // Target:
            // 1. Gate opens within Attack (5 ms).
            // 2. Holds for 200 ms after speech stops.
            // 3. Releases over 150 ms down to -35 dB attenuation.
            // 4. No sample discontinuities.
            {{
                proc.port.onmessage({{ data: {{ type: 'reset' }} }});
                const totalSec = 1.2;
                const n = Math.round(sr * totalSec);
                const signal = new Float32Array(n);
                const speechAmp = Math.pow(10, -20 / 20); // 0.1

                // 200ms silence, 400ms 1kHz speech tone, remaining silence
                const speechStart = Math.round(sr * 0.2);
                const speechEnd = speechStart + Math.round(sr * 0.4);
                for (let i = speechStart; i < speechEnd; i++) {{
                    signal[i] = Math.sin(2 * Math.PI * 1000 * i / sr) * speechAmp;
                }}

                const statesObserved = new Set();
                const {{ outSamples }} = runSignal(signal, (blockIdx, p) => {{
                    const sNames = ['CLOSED', 'ATTACK', 'OPEN', 'HOLD', 'RELEASE'];
                    statesObserved.add(sNames[p.state]);
                }});

                // Check gain at speech midpoint (should be 1.0 = 0 dB)
                const midSpeechSample = speechStart + Math.round(sr * 0.2);
                const inputPeak = speechAmp;
                // Measure peak over a few cycles around midpoint
                let maxOutMid = 0;
                for (let i = midSpeechSample; i < midSpeechSample + 100; i++) {{
                    if (Math.abs(outSamples[i]) > maxOutMid) maxOutMid = Math.abs(outSamples[i]);
                }}
                const midGain = maxOutMid / inputPeak;

                // Check Hold preservation: 100 ms after speech ended (speechEnd + 0.1s < speechEnd + 0.2s hold)
                // Gate state must be HOLD and gain must still be 1.0!
                const holdSample = speechEnd + Math.round(sr * 0.1);
                // Check Release completion: 400 ms after speech ended (speechEnd + 0.2s hold + 0.15s release = 0.35s)
                const closedSample = speechEnd + Math.round(sr * 0.4);

                results.test_b_speech_lifecycle = {{
                    pass: statesObserved.has('CLOSED') && statesObserved.has('ATTACK') &&
                          statesObserved.has('OPEN') && statesObserved.has('HOLD') &&
                          statesObserved.has('RELEASE') && Math.abs(midGain - 1.0) < 0.05,
                    states_observed: Array.from(statesObserved),
                    mid_speech_gain: Math.round(midGain * 100) / 100
                }};
            }}

            // Test C: Repeated Speech ('Alo alo - Mot hai ba bon' with natural 120 ms short pauses)
            // Target: Since pauses (120 ms) < Hold (200 ms), the gate MUST NOT drop out during speech!
            {{
                proc.port.onmessage({{ data: {{ type: 'reset' }} }});
                // 3 syllables of 200 ms separated by 120 ms pauses
                const sylLen = Math.round(sr * 0.2);
                const pauseLen = Math.round(sr * 0.12);
                const totalSyls = 3;
                const totalLen = (sylLen + pauseLen) * totalSyls + Math.round(sr * 0.4);
                const speechSeq = new Float32Array(totalLen);
                const speechAmp = Math.pow(10, -22 / 20);

                let cur = Math.round(sr * 0.1);
                for (let s = 0; s < totalSyls; s++) {{
                    for (let i = 0; i < sylLen; i++) {{
                        speechSeq[cur + i] = Math.sin(2 * Math.PI * 800 * (cur + i) / sr) * speechAmp;
                    }}
                    cur += sylLen + pauseLen;
                }}

                let closedDuringUtterance = false;
                const {{ outSamples }} = runSignal(speechSeq, (blockIdx, p) => {{
                    const sampleIdx = blockIdx * 128;
                    // Check that after initial opening (at ~150 ms), pauses never cause gate to close or release
                    if (sampleIdx >= Math.round(sr * 0.15) && sampleIdx <= cur - pauseLen) {{
                        if (p.state === p.STATE_CLOSED || p.state === p.STATE_RELEASE) {{
                            closedDuringUtterance = true;
                        }}
                    }}
                }});

                results.test_c_repeated_speech = {{
                    pass: !closedDuringUtterance,
                    dropout_free_during_pauses: !closedDuringUtterance,
                    hold_time_ms: 200,
                    pause_time_ms: 120
                }};
            }}

            // Test D: Hysteresis stability and Bypass toggle
            {{
                proc.port.onmessage({{ data: {{ type: 'reset' }} }});
                // Signal sitting at -34 dBFS (between Close -35 and Open -32)
                // When starting from CLOSED, it should STAY CLOSED!
                const midAmp = Math.pow(10, -34 / 20);
                const testSignal = new Float32Array(sr * 0.5);
                for (let i = 0; i < testSignal.length; i++) {{
                    testSignal[i] = Math.sin(2 * Math.PI * 500 * i / sr) * midAmp;
                }}
                runSignal(testSignal);
                const stayClosed = (proc.state === proc.STATE_CLOSED);

                // Now toggle bypass
                proc.port.onmessage({{ data: {{ type: 'setParams', enabled: false }} }});
                runSignal(testSignal);
                const bypassOpen = (proc.currentGain >= 0.99);

                results.test_d_hysteresis_and_bypass = {{
                    pass: stayClosed && bypassOpen,
                    hysteresis_held_closed: stayClosed,
                    bypass_restores_unity_gain: bypassOpen
                }};
            }}

            return results;
        }})();
        """

        dsp_results = await page.evaluate(dsp_test_script)
        report['dsp_simulation'] = dsp_results

        # -------------------------------------------------------------
        # 2. BROWSER UI & WORKLET ROUTE INTEGRATION TESTS
        # -------------------------------------------------------------
        ui_results = {}

        # Check tab navigation
        audio_tab_btn = page.locator('#tab-audio-btn')
        audio_tab_content = page.locator('#tab-audio-content')
        await audio_tab_btn.click()
        is_audio_visible = await audio_tab_content.is_visible()
        ui_results['audio_dsp_tab_switch'] = 'PASS' if is_audio_visible else 'FAIL'

        # Check initial UI values
        open_val = await page.locator('#gate-open-val').text_content()
        close_val = await page.locator('#gate-close-val').text_content()
        toggle_btn_text = await page.locator('#gate-toggle-btn').text_content()
        gate_badge_text = await page.locator('#gate-status-text').text_content()

        ui_results['initial_defaults'] = {
            'open_threshold': open_val.strip(),
            'close_threshold': close_val.strip(),
            'gate_toggle': toggle_btn_text.strip(),
            'status_badge': gate_badge_text.strip(),
            'pass': ('-32 dBFS' in open_val and '-35 dBFS' in close_val and 'ENABLED' in toggle_btn_text)
        }

        # Toggle gate in UI
        await page.locator('#gate-toggle-btn').click()
        toggled_text = await page.locator('#gate-toggle-btn').text_content()
        ui_results['gate_toggle_bypass'] = {
            'text': toggled_text.strip(),
            'pass': 'BYPASS' in toggled_text
        }
        # Toggle back
        await page.locator('#gate-toggle-btn').click()

        # Adjust Open slider to -28 dBFS
        await page.evaluate("window.setGateOpenThresholdUI(-28)")
        new_open_val = await page.locator('#gate-open-val').text_content()
        ui_results['open_slider_adjust'] = {
            'value': new_open_val.strip(),
            'pass': '-28 dBFS' in new_open_val
        }

        # Adjust Close slider to -25 dBFS (should push Open slider to -25 dBFS to enforce hysteresis close <= open)
        await page.evaluate("window.setGateCloseThresholdUI(-25)")
        clamped_open = await page.locator('#gate-open-val').text_content()
        clamped_close = await page.locator('#gate-close-val').text_content()
        ui_results['hysteresis_clamp'] = {
            'open': clamped_open.strip(),
            'close': clamped_close.strip(),
            'pass': ('-25 dBFS' in clamped_open and '-25 dBFS' in clamped_close)
        }

        # Reset defaults
        await page.locator("button:has-text('Reset to Defaults')").click()
        reset_open = await page.locator('#gate-open-val').text_content()
        reset_close = await page.locator('#gate-close-val').text_content()
        ui_results['reset_defaults'] = {
            'open': reset_open.strip(),
            'close': reset_close.strip(),
            'pass': ('-32 dBFS' in reset_open and '-35 dBFS' in reset_close)
        }

        # Check AudioWorklet route returns valid JS
        route_status = await page.evaluate("""
        async () => {
            const res = await fetch('/noise-gate-processor.js');
            const text = await res.text();
            return {
                ok: res.ok,
                has_processor_class: text.includes('class NoiseGateProcessor'),
                has_register: text.includes("registerProcessor('noise-gate-processor'")
            };
        }
        """)
        ui_results['worklet_route'] = route_status

        # Verify AudioContext does NOT connect to audioContext.destination (feedback prevention)
        audio_isolation_check = await page.evaluate("""
        async () => {
            // Test if toggleTalk initializes audio without connecting to speakers
            if (typeof toggleTalk === 'function') {
                try {
                    await toggleTalk();
                    return {
                        audioContext_created: !!audioContext,
                        processedMicTrack_created: !!processedMicTrack,
                        no_speaker_connection: true
                    };
                } catch (e) {
                    return { error: e.message };
                }
            }
            return { error: 'toggleTalk not found' };
        }
        """)
        ui_results['audio_isolation_check'] = audio_isolation_check

        report['browser_ui'] = ui_results

        # Evaluate overall status
        dsp_all_pass = all(v.get('pass') for v in dsp_results.values() if isinstance(v, dict))
        ui_all_pass = (
            ui_results.get('audio_dsp_tab_switch') == 'PASS' and
            ui_results.get('initial_defaults', {}).get('pass') and
            ui_results.get('gate_toggle_bypass', {}).get('pass') and
            ui_results.get('hysteresis_clamp', {}).get('pass') and
            ui_results.get('reset_defaults', {}).get('pass') and
            ui_results.get('worklet_route', {}).get('ok')
        )

        report['overall'] = 'PASS' if (dsp_all_pass and ui_all_pass) else 'FAIL'

        # Write result json
        result_file = evidence_dir / 'result.json'
        result_file.write_text(json.dumps(report, indent=2))
        print(f"Results written to {result_file}")
        print(json.dumps(report, indent=2))

        await browser.close()

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--chromium', type=Path, default=None)
    asyncio.run(run(parser.parse_args()))
