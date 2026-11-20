// Exercise the actual client microphone path and Talk controls without hardware.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const elements = new Map();
const mic = { enabled: true };
const silent = { enabled: true };
const filtered = { enabled: true };
let contextCount = 0;
let socket;
let outgoing;
let constraints;
let permissionRequests = 0;
const mixer = new Map([['/api/earbud-volume', 12], ['/api/earbud-mic-volume', 15], ['/api/earbud-mic-boost', 200]]);
const sender = { replaceTrack: async track => { outgoing = track; } };
const context = {
    console,
    location: { protocol: 'https:', hostname: 'maixcam.test' },
    document: {
        getElementById(id) {
            if (!elements.has(id)) elements.set(id, {});
            return elements.get(id);
        },
        addEventListener() {},
    },
    navigator: { mediaDevices: { getUserMedia: async options => {
        constraints = options;
        permissionRequests++;
        return { getAudioTracks: () => [mic], getTracks: () => [mic] };
    } } },
    isSecureContext: true,
    addEventListener() {},
    setInterval() {}, clearInterval() {}, setTimeout() {},
    fetch: async (url, options) => {
        if (mixer.has(url) && options?.method === 'POST') mixer.set(url, Number(options.body));
        return { ok: true, json: async () => ({ volume: mixer.get(url) }) };
    },
    AudioContext: class {
        constructor() { this.filteredContext = ++contextCount > 1; this.sampleRate = 48000; this.audioWorklet = { addModule: async () => {} }; }
        createMediaStreamDestination() { return { stream: { getAudioTracks: () => [this.filteredContext ? filtered : silent] } }; }
        createMediaStreamSource() { return { connect: node => node }; }
        async resume() {}
        async close() {}
    },
    AudioWorkletNode: class { constructor() { this.port = { postMessage() {} }; } connect() {} },
    WebSocket: class {
        constructor() { socket = this; }
        send() {}
    },
    RTCPeerConnection: class {
        iceGatheringState = 'complete';
        localDescription = { type: 'answer', sdp: '' };
        async setRemoteDescription() {}
        async createAnswer() { return this.localDescription; }
        async setLocalDescription() {}
        getTransceivers() { return [{ receiver: { track: { kind: 'audio' } }, sender }]; }
    },
};
context.window = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname, '../../src/client.js'), 'utf8'), context);
(async () => {
    await socket.onmessage({ data: JSON.stringify({ type: 'offer', sdp: '' }) });
    assert.equal(outgoing, silent);
    const micVolume = elements.get('earbud-mic-volume');
    assert.equal(micVolume.value, 15);
    micVolume.value = '8';
    await micVolume.onchange();
    assert.equal(elements.get('earbud-mic-volume-value').textContent, '53%');
    assert.equal(mixer.get('/api/earbud-volume'), 12);
    assert.equal(elements.get('earbud-mic-boost-value').textContent, '200%');
    const talk = elements.get('talk-btn');
    await talk.onclick();
    assert.equal(constraints.audio.echoCancellation, false);
    assert.equal(constraints.audio.noiseSuppression, false);
    assert.equal(constraints.audio.autoGainControl, false);
    assert.equal(outgoing, filtered, 'Transmit the filtered microphone track');
    assert.equal(elements.get('noise-status').textContent, 'RNNoise active');
    await talk.onclick();
    assert.equal(outgoing, silent);
    assert.equal(mic.enabled, false);
    assert.equal(elements.get('noise-status').textContent, 'Talk is off');
    await talk.onclick();
    assert.equal(outgoing, filtered);
    assert.equal(mic.enabled, true);
    assert.equal(permissionRequests, 1);
    console.log('Filtered browser microphone and Talk controls: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
