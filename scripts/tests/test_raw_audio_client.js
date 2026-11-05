// Exercise the actual client microphone path and Talk controls without hardware.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const elements = new Map();
const mic = { enabled: true };
const silent = { enabled: true };
let socket;
let outgoing;
let constraints;
let permissionRequests = 0;
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
    fetch: async () => ({ ok: true, json: async () => ({}) }),
    AudioContext: class {
        createMediaStreamDestination() { return { stream: { getAudioTracks: () => [silent] } }; }
    },
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
    const talk = elements.get('talk-btn');
    await talk.onclick();
    assert.equal(constraints.audio.echoCancellation, false);
    assert.equal(constraints.audio.noiseSuppression, false);
    assert.equal(constraints.audio.autoGainControl, false);
    assert.equal(outgoing, mic, 'Transmit the original microphone track directly');
    await talk.onclick();
    assert.equal(outgoing, silent);
    assert.equal(mic.enabled, false);
    await talk.onclick();
    assert.equal(outgoing, mic);
    assert.equal(mic.enabled, true);
    assert.equal(permissionRequests, 1);
    console.log('Raw browser microphone and Talk controls: PASS');
})().catch(error => { console.error(error); process.exitCode = 1; });
