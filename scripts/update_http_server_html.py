#!/usr/bin/env python3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def main():
    html_file = ROOT / 'src/index.html'
    server_header = ROOT / 'src/maix_webrtc_http_server.hpp'

    with open(html_file, 'r', encoding='utf-8') as f:
        html_content = f.read()

    client_js = r'''
    const clientId = randomId(10);
    const wsUrl = 'ws://' + window.location.hostname + ':8001/' + clientId;
    console.log('[WebRTC] Connecting to:', wsUrl);
    const websocket = new WebSocket(wsUrl);

    let pc = null;
    let dc = null;
    let localStream = null;
    let localMicTrack = null;
    let silentAudioTrack = null;
    let audioTransceiver = null;
    let isListening = false;
    let isTalking = false;

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

    async function setTalkingState(enable) {
        if (enable === isTalking) return;
        if (enable) {
            try {
                if (!localStream) {
                    localStream = await navigator.mediaDevices.getUserMedia({
                        audio: {
                            sampleRate: 48000,
                            channelCount: 1,
                            echoCancellation: false,
                            noiseSuppression: false,
                            autoGainControl: false
                        },
                        video: false
                    });
                }
                localMicTrack = localStream.getAudioTracks()[0];
                if (audioTransceiver && localMicTrack) {
                    await audioTransceiver.sender.replaceTrack(localMicTrack);
                    isTalking = true;
                    if (talkBtn) {
                        talkBtn.className = 'call-btn btn-mic-live';
                        talkBtn.innerHTML = MIC_LIVE_SVG;
                        talkBtn.title = 'Operator Microphone Live (Click to Mute)';
                    }
                }
            } catch (err) {
                console.error('[Audio] Failed to access operator microphone:', err);
                alert('Microphone access denied: ' + err.message);
            }
        } else {
            const silent = getSilentTrack();
            if (audioTransceiver && silent) {
                await audioTransceiver.sender.replaceTrack(silent);
            }
            if (localMicTrack) {
                localMicTrack.stop();
                localMicTrack = null;
            }
            localStream = null;
            isTalking = false;
            if (talkBtn) {
                talkBtn.className = 'call-btn btn-mic-muted';
                talkBtn.innerHTML = MIC_MUTED_SVG;
                talkBtn.title = 'Operator Microphone Muted (Click to Talk)';
            }
        }
    }

    function toggleTalk() {
        setTalkingState(!isTalking);
    }

    function toggleListen() {
        isListening = !isListening;
        if (remoteAudio) {
            remoteAudio.muted = !isListening;
            if (isListening) {
                remoteAudio.play().catch(e => console.warn('[Audio] Remote audio play blocked:', e));
            }
        }
        if (listenBtn) {
            if (isListening) {
                listenBtn.className = 'call-btn btn-listen-live';
                listenBtn.innerHTML = LISTEN_LIVE_SVG;
                listenBtn.title = 'Field Audio Live (Click to Mute)';
            } else {
                listenBtn.className = 'call-btn btn-listen-muted';
                listenBtn.innerHTML = LISTEN_MUTED_SVG;
                listenBtn.title = 'Field Audio Muted (Click to Listen)';
            }
        }
    }

    if (talkBtn) talkBtn.onclick = toggleTalk;
    if (listenBtn) listenBtn.onclick = toggleListen;

    function createPeerConnection() {
        const config = {
            iceServers: [{ urls: ['stun:stun.l.google.com:19302'] }]
        };
        const peer = new RTCPeerConnection(config);

        peer.onconnectionstatechange = () => {
            console.log('[WebRTC] PeerConnection state:', peer.connectionState);
            if (connBadge && connText) {
                if (peer.connectionState === 'connected') {
                    connBadge.className = 'status-pill live';
                    connText.textContent = 'Live Duplex';
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

        const silentTrack = getSilentTrack();
        if (silentTrack) {
            audioTransceiver = peer.addTransceiver(silentTrack, { direction: 'sendrecv' });
        } else {
            audioTransceiver = peer.addTransceiver('audio', { direction: 'sendrecv' });
        }

        peer.onicecandidate = (event) => {
            if (event.candidate) {
                websocket.send(JSON.stringify({
                    type: 'candidate',
                    candidate: event.candidate.candidate,
                    sdpMid: event.candidate.sdpMid,
                    sdpMLineIndex: event.candidate.sdpMLineIndex
                }));
            }
        };

        return peer;
    }

    websocket.onopen = () => {
        console.log('[WebRTC] WebSocket connected, requesting offer...');
        if (connBadge && connText) {
            connBadge.className = 'status-pill connecting';
            connText.textContent = 'Signaling...';
        }
        pc = createPeerConnection();
    };

    websocket.onmessage = async (event) => {
        const msg = JSON.parse(event.data);
        if (msg.type === 'offer') {
            await pc.setRemoteDescription(new RTCSessionDescription({ type: 'offer', sdp: msg.sdp }));
            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);
            websocket.send(JSON.stringify({ type: 'answer', sdp: answer.sdp }));
        } else if (msg.type === 'candidate' && msg.candidate) {
            try {
                await pc.addIceCandidate(new RTCIceCandidate({
                    candidate: msg.candidate,
                    sdpMid: msg.sdpMid,
                    sdpMLineIndex: msg.sdpMLineIndex
                }));
            } catch (e) {
                console.warn('[WebRTC] Error adding ICE candidate:', e);
            }
        }
    };

    websocket.onclose = () => {
        console.log('[WebRTC] WebSocket closed');
        if (connBadge && connText) {
            connBadge.className = 'status-pill disconnected';
            connText.textContent = 'Disconnected';
        }
    };

    function randomId(len) {
        const chars = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ';
        let res = '';
        for (let i = 0; i < len; i++) res += chars.charAt(Math.floor(Math.random() * chars.length));
        return res;
    }

    // Telemetry polling
    async function pollTelemetry() {
        try {
            const resp = await fetch('/api/stats');
            if (!resp.ok) return;
            const data = await resp.json();

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
    '''

    with open(server_header, 'r', encoding='utf-8') as f:
        orig = f.read()

    prefix = orig[:orig.find('static constexpr const char WEBRTC_INDEX_HTML[] = R\"HTML(')]
    mid_marker = ')HTML\";'
    mid_pos = orig.find(mid_marker)
    js_marker = 'static constexpr const char WEBRTC_CLIENT_JS[] = R\"JS('
    js_pos = orig.find(js_marker, mid_pos)
    end_marker = ')JS\";'
    end_pos = orig.find(end_marker, js_pos)
    suffix = orig[end_pos + len(end_marker):]

    new_content = (
        prefix
        + 'static constexpr const char WEBRTC_INDEX_HTML[] = R\"HTML(\n'
        + html_content
        + '\n)HTML\";\n\n    '
        + 'static constexpr const char WEBRTC_CLIENT_JS[] = R\"JS(\n'
        + client_js
        + '\n)JS\";'
        + suffix
    )

    with open(server_header, 'w', encoding='utf-8') as f:
        f.write(new_content)

    print(f'Successfully updated {server_header}')

if __name__ == '__main__':
    main()
