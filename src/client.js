const clientId = randomId(10);
    const signalToken = '__SIGNAL_TOKEN__';
    const wsUrl = (location.protocol === 'https:' ? 'wss://' : 'ws://') + window.location.hostname + ':8001/' + (signalToken ? signalToken + '/' : '') + clientId;
    console.log('[WebRTC] Connecting to:', wsUrl);
    const websocket = new WebSocket(wsUrl);

    let lastSignalingReply = Date.now();
    const heartbeat = setInterval(() => {
        if (websocket.readyState !== WebSocket.OPEN) return;
        if (Date.now() - lastSignalingReply > 20000) {
            websocket.close();
            return;
        }
        websocket.send(JSON.stringify({ type: 'ping' }));
    }, 5000);

    let pc = null;
    let dc = null;
    let localStream = null;
    let localMicTrack = null;
    let silentAudioTrack = null;
    let audioTransceiver = null;
    let isListening = false;
    let isTalking = false;

    // --- AUDIO WORKLET & NOISE GATE STATE ---
    let audioContext = null;
    let micSourceNode = null;
    let noiseGateNode = null;
    let micDestinationNode = null;
    let processedMicTrack = null;
    let noiseGateLastState = 'CLOSED';

    const noiseGateParams = {
        enabled: true,
        openThreshold: -32.0,    // dBFS
        closeThreshold: -35.0,   // dBFS
        rmsWindowMs: 20.0,       // ms
        attackMs: 5.0,           // ms
        holdMs: 200.0,           // ms
        releaseMs: 150.0,        // ms
        closedGainDb: -35.0      // dB
    };

    const NOISE_GATE_PROCESSOR_INLINE = `
class NoiseGateProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super(options);
        this.enabled = true;
        this.openThreshold = -32.0;
        this.closeThreshold = -35.0;
        this.rmsWindowMs = 20.0;
        this.attackMs = 5.0;
        this.holdMs = 200.0;
        this.releaseMs = 150.0;
        this.closedGainDb = -35.0;
        this.closedGainLinear = Math.pow(10.0, this.closedGainDb / 20.0);
        this.currentGain = this.closedGainLinear;

        this.STATE_CLOSED = 0;
        this.STATE_ATTACK = 1;
        this.STATE_OPEN = 2;
        this.STATE_HOLD = 3;
        this.STATE_RELEASE = 4;
        this.state = this.STATE_CLOSED;
        this.holdCounter = 0;

        this.maxWindowSamples = 4096;
        this.ringBuffer = new Float32Array(this.maxWindowSamples);
        this.windowSamples = Math.max(1, Math.min(this.maxWindowSamples, Math.round(sampleRate * (this.rmsWindowMs / 1000.0))));
        this.writeIdx = 0;
        this.runningSumSquares = 0.0;
        this.resyncCounter = 0;

        this.telemetryCounter = 0;
        this.telemetryIntervalBlocks = Math.max(1, Math.round((sampleRate * 0.08) / 128.0));
        this.lastDbfs = -140.0;

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
        if (!input || input.length === 0 || !input[0] || input[0].length === 0) {
            for (let c = 0; c < numOutChannels; c++) output[c].fill(0);
            return true;
        }
        const numInChannels = input.length;
        const numSamples = Math.min(input[0].length, outSamples);

        const attackSamples = Math.max(1, Math.round(sampleRate * (this.attackMs / 1000.0)));
        const releaseSamples = Math.max(1, Math.round(sampleRate * (this.releaseMs / 1000.0)));
        const holdSamplesTotal = Math.max(1, Math.round(sampleRate * (this.holdMs / 1000.0)));

        const attackStep = (1.0 - this.closedGainLinear) / attackSamples;
        const releaseStep = (1.0 - this.closedGainLinear) / releaseSamples;

        const epsilon = 1e-7;
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
            let sampleSqSum = 0.0;
            for (let c = 0; c < numInChannels; c++) {
                const s = input[c][i];
                sampleSqSum += s * s;
            }
            const currentSq = sampleSqSum * invInChannels;
            const oldSq = ring[wIdx];
            ring[wIdx] = currentSq;
            runSum += currentSq - oldSq;
            wIdx++;
            if (wIdx >= winSamples) wIdx = 0;

            let meanSq = runSum / winSamples;
            if (meanSq < 0.0) meanSq = 0.0;
            const rms = Math.sqrt(meanSq);
            const dbfs = 20.0 * Math.log10(rms > epsilon ? rms : epsilon);
            if (dbfs > blockMaxDbfs) blockMaxDbfs = dbfs;

            if (!this.enabled) {
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
                        if (dbfs >= this.openThreshold) st = this.STATE_ATTACK;
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
                            if (holdRemaining <= 0) st = this.STATE_RELEASE;
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

            for (let c = 0; c < numOutChannels; c++) {
                const inC = c < numInChannels ? c : 0;
                output[c][i] = input[inC][i] * curGain;
            }
        }

        this.resyncCounter++;
        if (this.resyncCounter >= 64) {
            this.resyncCounter = 0;
            let acc = 0.0;
            for (let j = 0; j < winSamples; j++) acc += ring[j];
            runSum = acc;
        }

        this.currentGain = curGain;
        this.state = st;
        this.holdCounter = holdRemaining;
        this.runningSumSquares = runSum;
        this.writeIdx = wIdx;
        this.lastDbfs = blockMaxDbfs;

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
`;

    const listenBtn = document.getElementById('listen-btn');
    const talkBtn = document.getElementById('talk-btn');
    const connBadge = document.getElementById('conn-status-badge');
    const connText = document.getElementById('conn-text');
    const remoteAudio = document.getElementById('remote-audio');
    const fullscreenBtn = document.getElementById('fullscreen-btn');

    const MIC_MUTED_SVG = '<svg class="icon" viewBox="0 0 24 24"><path d="m18.54 18.54a10.8 10.8 0 0 1-6.54 2.46c-5.52 0-10-4.48-10-10 0-2.45.88-4.7 2.34-6.46"/><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><line x1="1" x2="23" y1="1" y2="23"/></svg>';
    const MIC_LIVE_SVG = '<svg class="icon" viewBox="0 0 24 24"><path d="M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z"/><path d="M19 10v2a7 7 0 0 1-14 0v-2"/><line x1="12" x2="12" y1="19" y2="22"/></svg>';

    const LISTEN_MUTED_SVG = '<svg class="icon" viewBox="0 0 24 24"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07"/></svg>';
    const LISTEN_LIVE_SVG = '<svg class="icon" viewBox="0 0 24 24"><polygon points="11 5 6 9 2 9 2 15 6 15 11 19 11 5"/><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"/></svg>';

    function getSilentTrack() {
        if (!silentAudioTrack) {
            try {
                const ctx = new (window.AudioContext || window.webkitAudioContext)();
                const dest = ctx.createMediaStreamDestination();
                silentAudioTrack = dest.stream.getAudioTracks()[0];
                if (silentAudioTrack) {
                    silentAudioTrack.enabled = true;
                }
            } catch (e) {
                console.warn('[Audio] Could not create silent track:', e);
            }
        }
        return silentAudioTrack;
    }

    async function setupNoiseGate(stream) {
        if (!window.AudioContext && !window.webkitAudioContext) {
            console.warn('[Audio] Web Audio API unsupported in this browser');
            updateAudioProcessingUIStatus('Web Audio API unavailable');
            return null;
        }

        try {
            const AudioCtx = window.AudioContext || window.webkitAudioContext;
            if (!audioContext) {
                audioContext = new AudioCtx({ sampleRate: 48000 });
            }
            if (audioContext.state === 'suspended') {
                await audioContext.resume();
            }

            if (!noiseGateNode) {
                let moduleLoaded = false;
                try {
                    await audioContext.audioWorklet.addModule('/noise-gate-processor.js');
                    moduleLoaded = true;
                } catch (netErr) {
                    console.warn('[Audio] Network load of /noise-gate-processor.js failed, using inline blob fallback:', netErr);
                }

                if (!moduleLoaded) {
                    const blob = new Blob([NOISE_GATE_PROCESSOR_INLINE], { type: 'application/javascript' });
                    const blobUrl = URL.createObjectURL(blob);
                    try {
                        await audioContext.audioWorklet.addModule(blobUrl);
                        moduleLoaded = true;
                    } finally {
                        URL.revokeObjectURL(blobUrl);
                    }
                }

                noiseGateNode = new AudioWorkletNode(audioContext, 'noise-gate-processor');
                noiseGateNode.port.onmessage = (event) => {
                    if (event.data && event.data.type === 'telemetry') {
                        handleGateTelemetry(event.data);
                    }
                };
                noiseGateNode.port.postMessage({ type: 'setParams', ...noiseGateParams });
            }

            if (micSourceNode) {
                try { micSourceNode.disconnect(); } catch (e) {}
            }
            micSourceNode = audioContext.createMediaStreamSource(stream);

            if (!micDestinationNode) {
                micDestinationNode = audioContext.createMediaStreamDestination();
            }

            micSourceNode.connect(noiseGateNode);
            // CRITICAL: Connect ONLY to micDestinationNode, NEVER to audioContext.destination (avoids PC speaker feedback!)
            noiseGateNode.connect(micDestinationNode);

            processedMicTrack = micDestinationNode.stream.getAudioTracks()[0];
            updateAudioProcessingUIStatus('Active (AudioWorklet Hysteresis)');
            console.log('[Audio] Noise Gate initialized successfully with AudioWorklet');
            return processedMicTrack;
        } catch (e) {
            console.warn('[Audio] AudioWorklet noise gate failed, falling back to raw mic:', e);
            updateAudioProcessingUIStatus('Fallback to Direct Mic (' + e.message + ')');
            processedMicTrack = null;
            return null;
        }
    }

    function getAudioTrackToSend() {
        if (!isTalking) {
            return getSilentTrack();
        }
        if (processedMicTrack) {
            return processedMicTrack;
        }
        return localMicTrack || getSilentTrack();
    }

    function handleGateTelemetry(data) {
        if (!data) return;
        noiseGateLastState = data.state;
        if (isTalking) {
            updateGateStatusDisplay(true, data.state, data.gainDb, data.dbfs);
        }
    }

    function updateGateStatusDisplay(talking, state, gainDb, dbfs) {
        const badge = document.getElementById('gate-status-badge');
        const text = document.getElementById('gate-status-text');
        if (badge && text) {
            if (!talking) {
                text.textContent = 'Gate: Idle';
                badge.className = 'status-pill';
            } else {
                text.textContent = 'Gate: ' + state + (gainDb !== undefined ? ' (' + (gainDb >= 0 ? '+' : '') + gainDb.toFixed(1) + ' dB)' : '');
                if (state === 'OPEN') {
                    badge.className = 'status-pill connected';
                } else if (state === 'ATTACK' || state === 'HOLD' || state === 'RELEASE') {
                    badge.className = 'status-pill connecting';
                } else {
                    badge.className = 'status-pill';
                }
            }
        }

        const statePill = document.getElementById('gate-state-pill');
        const gainPill = document.getElementById('gate-gain-pill');
        const meterFill = document.getElementById('gate-meter-fill');
        const meterText = document.getElementById('gate-meter-text');

        if (statePill) {
            statePill.textContent = 'State: ' + state;
            if (state === 'OPEN') {
                statePill.style.background = 'var(--pill-positive-bg)';
                statePill.style.color = 'var(--pill-positive-ink)';
            } else if (state === 'ATTACK' || state === 'HOLD' || state === 'RELEASE') {
                statePill.style.background = 'var(--pill-warning-bg)';
                statePill.style.color = 'var(--pill-warning-ink)';
            } else {
                statePill.style.background = 'var(--pill-neutral-bg)';
                statePill.style.color = 'var(--pill-neutral-ink)';
            }
        }

        if (gainPill && gainDb !== undefined) {
            gainPill.textContent = 'Gain: ' + (gainDb >= 0 ? '+' : '') + gainDb.toFixed(1) + ' dB';
        }

        if (meterFill && dbfs !== undefined) {
            const pct = Math.max(0, Math.min(100, ((dbfs + 60.0) / 60.0) * 100.0));
            meterFill.style.width = pct + '%';
            if (pct > 70) {
                meterFill.style.background = 'var(--warning)';
            } else if (pct > 40) {
                meterFill.style.background = 'var(--positive)';
            } else {
                meterFill.style.background = 'var(--accent)';
            }
        }

        if (meterText && dbfs !== undefined) {
            meterText.textContent = (dbfs <= -100 ? '-inf' : dbfs.toFixed(1)) + ' dBFS';
        }
    }

    function updateAudioProcessingUIStatus(status) {
        const el = document.getElementById('gate-pipeline-status');
        if (el) el.textContent = status;
    }

    // Window-exposed UI controls
    window.toggleNoiseGateUI = function() {
        noiseGateParams.enabled = !noiseGateParams.enabled;
        if (noiseGateNode) {
            noiseGateNode.port.postMessage({ type: 'setParams', enabled: noiseGateParams.enabled });
        }
        const btn = document.getElementById('gate-toggle-btn');
        if (btn) {
            btn.textContent = 'Noise Gate: ' + (noiseGateParams.enabled ? 'ENABLED' : 'BYPASS');
            btn.style.color = noiseGateParams.enabled ? 'var(--positive)' : 'var(--warning)';
        }
        if (!noiseGateParams.enabled) {
            updateGateStatusDisplay(isTalking, 'BYPASS', 0.0);
        }
    };

    window.setGateOpenThresholdUI = function(val) {
        const num = parseFloat(val);
        if (isNaN(num)) return;
        noiseGateParams.openThreshold = num;
        if (noiseGateParams.openThreshold < noiseGateParams.closeThreshold) {
            noiseGateParams.closeThreshold = noiseGateParams.openThreshold;
            const closeSlider = document.getElementById('gate-close-slider');
            const closeVal = document.getElementById('gate-close-val');
            if (closeSlider) closeSlider.value = noiseGateParams.closeThreshold;
            if (closeVal) closeVal.textContent = noiseGateParams.closeThreshold + ' dBFS';
        }
        const valEl = document.getElementById('gate-open-val');
        if (valEl) valEl.textContent = noiseGateParams.openThreshold + ' dBFS';
        if (noiseGateNode) {
            noiseGateNode.port.postMessage({ type: 'setParams', openThreshold: noiseGateParams.openThreshold, closeThreshold: noiseGateParams.closeThreshold });
        }
    };

    window.setGateCloseThresholdUI = function(val) {
        const num = parseFloat(val);
        if (isNaN(num)) return;
        noiseGateParams.closeThreshold = num;
        if (noiseGateParams.closeThreshold > noiseGateParams.openThreshold) {
            noiseGateParams.openThreshold = noiseGateParams.closeThreshold;
            const openSlider = document.getElementById('gate-open-slider');
            const openVal = document.getElementById('gate-open-val');
            if (openSlider) openSlider.value = noiseGateParams.openThreshold;
            if (openVal) openVal.textContent = noiseGateParams.openThreshold + ' dBFS';
        }
        const valEl = document.getElementById('gate-close-val');
        if (valEl) valEl.textContent = noiseGateParams.closeThreshold + ' dBFS';
        if (noiseGateNode) {
            noiseGateNode.port.postMessage({ type: 'setParams', openThreshold: noiseGateParams.openThreshold, closeThreshold: noiseGateParams.closeThreshold });
        }
    };

    window.resetGateDefaultsUI = function() {
        noiseGateParams.enabled = true;
        noiseGateParams.openThreshold = -32.0;
        noiseGateParams.closeThreshold = -35.0;
        noiseGateParams.rmsWindowMs = 20.0;
        noiseGateParams.attackMs = 5.0;
        noiseGateParams.holdMs = 200.0;
        noiseGateParams.releaseMs = 150.0;
        noiseGateParams.closedGainDb = -35.0;

        if (noiseGateNode) {
            noiseGateNode.port.postMessage({ type: 'reset' });
        }

        const btn = document.getElementById('gate-toggle-btn');
        if (btn) {
            btn.textContent = 'Noise Gate: ENABLED';
            btn.style.color = 'var(--positive)';
        }
        const openSlider = document.getElementById('gate-open-slider');
        const openVal = document.getElementById('gate-open-val');
        if (openSlider) openSlider.value = -32;
        if (openVal) openVal.textContent = '-32 dBFS';

        const closeSlider = document.getElementById('gate-close-slider');
        const closeVal = document.getElementById('gate-close-val');
        if (closeSlider) closeSlider.value = -35;
        if (closeVal) closeVal.textContent = '-35 dBFS';
    };

    websocket.onopen = () => {
        console.log('[WebRTC] WebSocket connected, requesting offer...');
        if (connBadge && connText) {
            connBadge.className = 'status-pill connecting';
            connText.textContent = 'Signaling Connected';
        }
        sendRequest();
    };

    websocket.onclose = () => {
        console.warn('[WebRTC] WebSocket closed');
        clearInterval(heartbeat);
        stopTalk();
        if (localStream) localStream.getTracks().forEach(track => track.stop());
        if (pc) pc.close();
        setTimeout(() => location.reload(), 4000 + Math.random() * 2000);
        if (connBadge && connText) {
            connBadge.className = 'status-pill disconnected';
            connText.textContent = 'Signaling Closed';
        }
    };

    websocket.onerror = (err) => {
        console.error('[WebRTC] WebSocket error:', err);
        if (connBadge && connText) {
            connBadge.className = 'status-pill disconnected';
            connText.textContent = 'Signaling Error';
        }
    };

    websocket.onmessage = async (evt) => {
        if (typeof evt.data !== 'string') return;
        const message = JSON.parse(evt.data);
        lastSignalingReply = Date.now();
        if (message.type === 'busy' && connText) connText.textContent = 'Device in use';
        if (message.type === 'offer') {
            await handleOffer(message);
        }
    };

    function createPeerConnection() {
        const config = {
            bundlePolicy: 'max-bundle',
            iceServers: [],
        };

        const peer = new RTCPeerConnection(config);

        peer.onconnectionstatechange = () => {
            console.log('[WebRTC] PeerConnection state:', peer.connectionState);
            if (peer.connectionState === 'failed') websocket.close();
            if (connBadge && connText) {
                if (peer.connectionState === 'connected') {
                    connBadge.className = 'status-pill connected';
                    connText.textContent = 'WebRTC Live';
                } else if (peer.connectionState === 'connecting') {
                    connBadge.className = 'status-pill connecting';
                    connText.textContent = 'Connecting...';
                } else if (peer.connectionState === 'failed' || peer.connectionState === 'disconnected') {
                    connBadge.className = 'status-pill disconnected';
                    connText.textContent = 'Offline';
                }
            }
        };

        peer.ontrack = (evt) => {
            if (evt.track.kind === 'video') {
                const video = document.getElementById('video');
                video.srcObject = evt.streams[0] || new MediaStream([evt.track]);
                video.play().catch(e => console.warn('[Video] Auto-play blocked:', e));

                video.addEventListener('loadedmetadata', () => {
                    const res = document.getElementById('res-badge');
                    if (res) res.textContent = video.videoWidth + ' × ' + video.videoHeight + ' @ 30fps';
                    if (typeof window.applyRotation === 'function') {
                        window.applyRotation();
                    }
                });
            } else if (evt.track.kind === 'audio') {
                if (remoteAudio) {
                    remoteAudio.srcObject = evt.streams[0] || new MediaStream([evt.track]);
                    remoteAudio.muted = !isListening;
                }
            }
        };

        peer.ondatachannel = (evt) => {
            dc = evt.channel;
            dc.onopen = () => console.log('[DataChannel] Opened');
            dc.onclose = () => console.log('[DataChannel] Closed');
            dc.onmessage = (e) => console.log('[DataChannel] Message:', e.data);
        };

        return peer;
    }

    async function waitGatheringComplete() {
        return new Promise((resolve) => {
            if (pc.iceGatheringState === 'complete') resolve();
            else {
                const check = () => {
                    if (pc.iceGatheringState === 'complete') {
                        pc.removeEventListener('icegatheringstatechange', check);
                        resolve();
                    }
                };
                pc.addEventListener('icegatheringstatechange', check);
                setTimeout(resolve, 2000);
            }
        });
    }

    async function sendAnswer(peer) {
        await peer.setLocalDescription(await peer.createAnswer());
        await waitGatheringComplete();

        const answer = peer.localDescription;
        websocket.send(JSON.stringify({
            id: 'server',
            type: answer.type,
            sdp: answer.sdp,
        }));
    }

    async function handleOffer(offer) {
        pc = createPeerConnection();
        await pc.setRemoteDescription(offer);

        audioTransceiver = pc.getTransceivers().find(
            t => t.receiver && t.receiver.track && t.receiver.track.kind === 'audio'
        );

        if (audioTransceiver) {
            audioTransceiver.direction = 'sendrecv';
            const trackToSend = isTalking ? getAudioTrackToSend() : getSilentTrack();
            if (trackToSend && audioTransceiver.sender) {
                try {
                    await audioTransceiver.sender.replaceTrack(trackToSend);
                } catch (e) {
                    console.warn('[WebRTC] Initial track replace warning:', e);
                }
            }
        }

        await sendAnswer(pc);
    }

    function sendRequest() {
        websocket.send(JSON.stringify({ id: 'server', type: 'request' }));
    }

    function randomId(length) {
        const chars = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
        return [...Array(length)].map(() => chars.charAt(Math.floor(Math.random() * chars.length))).join('');
    }

    // --- CALL-APP LISTEN BUTTON (SPEAKER TOGGLE) ---
    if (listenBtn) {
        listenBtn.onclick = async () => {
            isListening = !isListening;
            if (remoteAudio) {
                remoteAudio.muted = !isListening;
                if (isListening) {
                    try {
                        await remoteAudio.play();
                        listenBtn.className = 'call-btn btn-listen-active';
                        listenBtn.innerHTML = LISTEN_LIVE_SVG;
                        listenBtn.title = 'Mute Field Audio';
                    } catch (e) {
                        console.warn('[Audio] Listen playback blocked by browser:', e);
                        isListening = false;
                        remoteAudio.muted = true;
                        listenBtn.className = 'call-btn btn-listen-muted';
                        listenBtn.innerHTML = LISTEN_MUTED_SVG;
                        listenBtn.title = 'Listen to Field Audio';
                    }
                } else {
                    listenBtn.className = 'call-btn btn-listen-muted';
                    listenBtn.innerHTML = LISTEN_MUTED_SVG;
                    listenBtn.title = 'Listen to Field Audio';
                }
            }
        };
    }

    // --- CALL-APP TALK BUTTON (SEAMLESS MIC TOGGLE WITH AUDIO WORKLET NOISE GATE) ---
    async function toggleTalk() {
        if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia) {
            alert('Microphone requires HTTPS with the MaixCAM certificate authority trusted on this device.');
            return;
        }
        if (!localStream || !localMicTrack) {
            try {
                localStream = await navigator.mediaDevices.getUserMedia({
                    audio: {
                        echoCancellation: true,
                        noiseSuppression: true,
                        autoGainControl: false
                    },
                    video: false
                });

                localMicTrack = localStream.getAudioTracks()[0];
                localMicTrack.enabled = true;

                await setupNoiseGate(localStream);

                const trackToSend = getAudioTrackToSend();
                if (trackToSend) trackToSend.enabled = true;

                if (audioTransceiver && audioTransceiver.sender && trackToSend) {
                    await audioTransceiver.sender.replaceTrack(trackToSend);
                }

                startTalk();
            } catch (err) {
                console.error('[Audio] Microphone permission error:', err);
                alert('Microphone permission required to talk: ' + err.message);
            }
            return;
        }

        if (isTalking) {
            stopTalk();
        } else {
            startTalk();
        }
    }

    function notifyTalkState(active) {
        // 1. WebRTC DataChannel message
        if (dc && dc.readyState === 'open') {
            try {
                dc.send(JSON.stringify({ type: active ? 'talk-start' : 'talk-stop' }));
            } catch (e) {
                console.warn('[DataChannel] Talk send failed:', e);
            }
        }
        // 2. HTTP GET notification (verified 100% reliable 200 OK)
        fetch('/api/talk?active=' + (active ? '1' : '0')).catch(() => {});
        // 3. HTTP POST fallback with json body
        fetch('/api/talk', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ active: active })
        }).catch(() => {});
    }

    async function startTalk() {
        if (isTalking) return;
        isTalking = true;

        if (audioContext && audioContext.state === 'suspended') {
            try { await audioContext.resume(); } catch (e) {}
        }

        if (localMicTrack) localMicTrack.enabled = true;
        if (processedMicTrack) processedMicTrack.enabled = true;

        const trackToSend = getAudioTrackToSend();
        if (trackToSend) trackToSend.enabled = true;

        if (audioTransceiver && audioTransceiver.sender && trackToSend) {
            try {
                await audioTransceiver.sender.replaceTrack(trackToSend);
            } catch (e) {
                console.warn('[Audio] replaceTrack to mic error:', e);
            }
        }

        if (talkBtn) {
            talkBtn.className = 'call-btn btn-mic-live';
            talkBtn.innerHTML = MIC_LIVE_SVG;
            talkBtn.title = 'Mute Mic (Transmitting to Headset)';
        }

        updateGateStatusDisplay(true, noiseGateLastState);
        notifyTalkState(true);
    }

    async function stopTalk() {
        if (!isTalking) return;
        isTalking = false;

        if (processedMicTrack) {
            processedMicTrack.enabled = false;
        }
        if (localMicTrack) {
            localMicTrack.enabled = false;
        }

        const silent = getSilentTrack();
        if (silent && audioTransceiver && audioTransceiver.sender) {
            try {
                await audioTransceiver.sender.replaceTrack(silent);
            } catch (e) {
                console.warn('[Audio] replaceTrack to silent error:', e);
            }
        }

        if (talkBtn) {
            talkBtn.className = 'call-btn btn-mic-muted';
            talkBtn.innerHTML = MIC_MUTED_SVG;
            talkBtn.title = 'Unmute Mic (Talk to Headset)';
        }

        updateGateStatusDisplay(false, 'Idle');
        notifyTalkState(false);
    }

    if (talkBtn) {
        talkBtn.onclick = toggleTalk;
    }

    // Safety release: ONLY when tab is truly hidden / switched
    document.addEventListener('visibilitychange', () => {
        if (document.hidden && isTalking) {
            stopTalk();
        }
    });

    window.addEventListener('pagehide', () => {
        stopTalk();
        websocket.close();
    });

    // --- TELEMETRY POLLING ---
    async function pollTelemetry() {
        try {
            const res = await fetch('/api/audio_stats');
            if (!res.ok) return;
            const data = await res.json();

            if (data.uplink) {
                const upPkts = (data.uplink.packets_sent || 0).toLocaleString();
                const upKB = ((data.uplink.bytes_sent || 0) / 1024).toFixed(1);
                const el = document.getElementById('tel-uplink');
                if (el) el.textContent = upPkts + ' pkts (' + upKB + ' KB)';
            }

            if (data.downlink) {
                const rxPkts = (data.downlink.packets_received || 0).toLocaleString();
                const decPkts = (data.downlink.packets_decoded || 0).toLocaleString();
                const el = document.getElementById('tel-downlink');
                if (el) el.textContent = decPkts + ' / ' + rxPkts + ' pkts';

                if (data.downlink.playback) {
                    const frames = (data.downlink.playback.frames_written || 0).toLocaleString();
                    const underruns = data.downlink.playback.underruns || 0;
                    const elPlay = document.getElementById('tel-playback');
                    const elUnder = document.getElementById('tel-underruns');
                    if (elPlay) elPlay.textContent = frames + ' frames';
                    if (elUnder) elUnder.textContent = underruns + ' underruns';
                }
            }
        } catch (e) {
            // Server temporarily unreachable or rebooting
        }
    }
    setInterval(pollTelemetry, 1500);

    // Fullscreen toggle
    if (fullscreenBtn) {
        fullscreenBtn.onclick = () => {
            const container = document.querySelector('.video-container');
            if (!document.fullscreenElement) {
                container?.requestFullscreen?.() || container?.webkitRequestFullscreen?.();
            } else {
                document.exitFullscreen?.() || document.webkitExitFullscreen?.();
            }
        };
    }
