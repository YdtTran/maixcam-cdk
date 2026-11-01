/**
 * NoiseGateProcessor - Real-time AudioWorkletProcessor for PC -> MaixCAM audio uplink.
 *
 * Implements:
 * - Real-time RMS calculation over a sliding 20 ms window
 * - Hysteresis state machine: CLOSED -> ATTACK -> OPEN -> HOLD -> RELEASE -> CLOSED
 * - Default parameters:
 *     Open Threshold:  -32 dBFS
 *     Close Threshold: -35 dBFS
 *     RMS Window:      20 ms
 *     Attack:          5 ms
 *     Hold:            200 ms
 *     Release:         150 ms
 *     Closed Gain:     -35 dB (attenuation ~0.0178x, continuous audio preserved)
 * - Sample-rate aware linear gain smoothing
 * - Bounded, zero-allocation real-time process() loop
 * - Safe multi-channel support and throttled main-thread telemetry
 */

class NoiseGateProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super(options);

        // Configurable parameters with required defaults
        this.enabled = true;
        this.openThreshold = -32.0;    // dBFS
        this.closeThreshold = -35.0;   // dBFS
        this.rmsWindowMs = 20.0;       // ms
        this.attackMs = 5.0;           // ms
        this.holdMs = 200.0;           // ms
        this.releaseMs = 150.0;        // ms
        this.closedGainDb = -35.0;     // dB

        // Pre-computed gain values
        this.closedGainLinear = Math.pow(10.0, this.closedGainDb / 20.0);
        this.currentGain = this.closedGainLinear;

        // State Machine constants
        this.STATE_CLOSED = 0;
        this.STATE_ATTACK = 1;
        this.STATE_OPEN = 2;
        this.STATE_HOLD = 3;
        this.STATE_RELEASE = 4;
        this.state = this.STATE_CLOSED;

        // Timing / countdown counters
        this.holdCounter = 0;

        // Pre-allocated ring buffer for RMS sliding window calculation
        // 4096 samples supports up to 96 kHz at 40 ms window without reallocations
        this.maxWindowSamples = 4096;
        this.ringBuffer = new Float32Array(this.maxWindowSamples);
        this.windowSamples = Math.max(1, Math.min(this.maxWindowSamples, Math.round(sampleRate * (this.rmsWindowMs / 1000.0))));
        this.writeIdx = 0;
        this.runningSumSquares = 0.0;
        this.resyncCounter = 0;

        // Main-thread telemetry throttling (~80-100 ms interval)
        this.telemetryCounter = 0;
        this.telemetryIntervalBlocks = Math.max(1, Math.round((sampleRate * 0.08) / 128.0));
        this.lastDbfs = -140.0;

        // Handle control messages from main thread
        this.port.onmessage = (e) => {
            const data = e.data;
            if (!data) return;
            if (data.type === 'setParams') {
                if (typeof data.enabled === 'boolean') this.enabled = data.enabled;
                if (typeof data.openThreshold === 'number') this.openThreshold = data.openThreshold;
                if (typeof data.closeThreshold === 'number') this.closeThreshold = data.closeThreshold;
                if (typeof data.attackMs === 'number') this.attackMs = data.attackMs;
                if (typeof data.holdMs === 'number') this.holdMs = data.holdMs;
                if (typeof data.releaseMs === 'number') this.releaseMs = data.releaseMs;
                if (typeof data.closedGainDb === 'number') {
                    this.closedGainDb = data.closedGainDb;
                    this.closedGainLinear = Math.pow(10.0, this.closedGainDb / 20.0);
                }
                if (typeof data.rmsWindowMs === 'number') {
                    this.rmsWindowMs = data.rmsWindowMs;
                    this.updateWindowSize();
                }
            } else if (data.type === 'reset') {
                this.enabled = true;
                this.openThreshold = -32.0;
                this.closeThreshold = -35.0;
                this.attackMs = 5.0;
                this.holdMs = 200.0;
                this.releaseMs = 150.0;
                this.closedGainDb = -35.0;
                this.closedGainLinear = Math.pow(10.0, -35.0 / 20.0);
                this.rmsWindowMs = 20.0;
                this.updateWindowSize();
            }
        };
    }

    updateWindowSize() {
        const newSize = Math.max(1, Math.min(this.maxWindowSamples, Math.round(sampleRate * (this.rmsWindowMs / 1000.0))));
        if (newSize !== this.windowSamples) {
            this.ringBuffer.fill(0);
            this.writeIdx = 0;
            this.runningSumSquares = 0.0;
            this.windowSamples = newSize;
        }
    }

    process(inputs, outputs, parameters) {
        const input = inputs[0];
        const output = outputs[0];

        if (!output || output.length === 0) return true;

        const numOutChannels = output.length;
        const outSamples = output[0].length;

        // If no microphone input is active, output continuous zero/silence
        if (!input || input.length === 0 || !input[0] || input[0].length === 0) {
            for (let c = 0; c < numOutChannels; c++) {
                output[c].fill(0);
            }
            return true;
        }

        const numInChannels = input.length;
        const numSamples = Math.min(input[0].length, outSamples);

        // Precalculate ramp rates based on sample rate
        const attackSamples = Math.max(1, Math.round(sampleRate * (this.attackMs / 1000.0)));
        const releaseSamples = Math.max(1, Math.round(sampleRate * (this.releaseMs / 1000.0)));
        const holdSamplesTotal = Math.max(1, Math.round(sampleRate * (this.holdMs / 1000.0)));

        const attackStep = (1.0 - this.closedGainLinear) / attackSamples;
        const releaseStep = (1.0 - this.closedGainLinear) / releaseSamples;

        const epsilon = 1e-7; // -140 dBFS floor
        const invInChannels = 1.0 / numInChannels;
        const winSamples = this.windowSamples;
        const ring = this.ringBuffer;

        let curGain = this.currentGain;
        let st = this.state;
        let holdRemaining = this.holdCounter;
        let runSum = this.runningSumSquares;
        let wIdx = this.writeIdx;

        let blockMaxDbfs = -140.0;

        for (let i = 0; i < numSamples; i++) {
            // 1. Multi-channel mean square of current sample
            let sampleSqSum = 0.0;
            for (let c = 0; c < numInChannels; c++) {
                const s = input[c][i];
                sampleSqSum += s * s;
            }
            const currentSq = sampleSqSum * invInChannels;

            // 2. Sliding circular buffer update
            const oldSq = ring[wIdx];
            ring[wIdx] = currentSq;
            runSum += currentSq - oldSq;
            wIdx++;
            if (wIdx >= winSamples) {
                wIdx = 0;
            }

            // 3. RMS & dBFS calculation
            let meanSq = runSum / winSamples;
            if (meanSq < 0.0) meanSq = 0.0;
            const rms = Math.sqrt(meanSq);
            const dbfs = 20.0 * Math.log10(rms > epsilon ? rms : epsilon);

            if (dbfs > blockMaxDbfs) {
                blockMaxDbfs = dbfs;
            }

            // 4. State Machine / Gain computation
            if (!this.enabled) {
                // If bypassed, smoothly ramp gain to 1.0 (anti-click)
                if (curGain < 1.0) {
                    curGain += attackStep;
                    if (curGain > 1.0) curGain = 1.0;
                } else if (curGain > 1.0) {
                    curGain = 1.0;
                }
                st = this.STATE_OPEN;
            } else {
                switch (st) {
                    case this.STATE_CLOSED:
                        curGain = this.closedGainLinear;
                        if (dbfs >= this.openThreshold) {
                            st = this.STATE_ATTACK;
                        }
                        break;

                    case this.STATE_ATTACK:
                        curGain += attackStep;
                        if (curGain >= 1.0) {
                            curGain = 1.0;
                            st = this.STATE_OPEN;
                        }
                        break;

                    case this.STATE_OPEN:
                        curGain = 1.0;
                        if (dbfs < this.closeThreshold) {
                            st = this.STATE_HOLD;
                            holdRemaining = holdSamplesTotal;
                        }
                        break;

                    case this.STATE_HOLD:
                        curGain = 1.0;
                        if (dbfs >= this.closeThreshold) {
                            st = this.STATE_OPEN;
                        } else {
                            holdRemaining--;
                            if (holdRemaining <= 0) {
                                st = this.STATE_RELEASE;
                            }
                        }
                        break;

                    case this.STATE_RELEASE:
                        if (dbfs >= this.openThreshold) {
                            st = this.STATE_ATTACK;
                        } else {
                            curGain -= releaseStep;
                            if (curGain <= this.closedGainLinear) {
                                curGain = this.closedGainLinear;
                                st = this.STATE_CLOSED;
                            }
                        }
                        break;
                }
            }

            // 5. Apply smoothed gain to output channels
            for (let c = 0; c < numOutChannels; c++) {
                const inC = c < numInChannels ? c : 0;
                output[c][i] = input[inC][i] * curGain;
            }
        }

        // Periodic resync of running sum to eliminate float drift
        this.resyncCounter++;
        if (this.resyncCounter >= 64) {
            this.resyncCounter = 0;
            let acc = 0.0;
            for (let j = 0; j < winSamples; j++) {
                acc += ring[j];
            }
            runSum = acc;
        }

        this.currentGain = curGain;
        this.state = st;
        this.holdCounter = holdRemaining;
        this.runningSumSquares = runSum;
        this.writeIdx = wIdx;
        this.lastDbfs = blockMaxDbfs;

        // Throttled telemetry reporting to main thread
        this.telemetryCounter++;
        if (this.telemetryCounter >= this.telemetryIntervalBlocks) {
            this.telemetryCounter = 0;
            const stateNames = ['CLOSED', 'ATTACK', 'OPEN', 'HOLD', 'RELEASE'];
            this.port.postMessage({
                type: 'telemetry',
                dbfs: Math.round(blockMaxDbfs * 10) / 10,
                state: this.enabled ? stateNames[st] : 'BYPASS',
                gainDb: Math.round(20.0 * Math.log10(Math.max(curGain, epsilon)) * 10) / 10,
                enabled: this.enabled
            });
        }

        return true;
    }
}

registerProcessor('noise-gate-processor', NoiseGateProcessor);
