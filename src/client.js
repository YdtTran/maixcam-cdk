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

    function getAudioTrackToSend() {
        if (!isTalking) {
            return getSilentTrack();
        }
        return localMicTrack || getSilentTrack();
    }

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

    // --- CALL-APP TALK BUTTON (DIRECT MICROPHONE TRACK) ---
    async function toggleTalk() {
        if (!window.isSecureContext || !navigator.mediaDevices?.getUserMedia) {
            alert('Microphone requires HTTPS with the MaixCAM certificate authority trusted on this device.');
            return;
        }
        if (!localStream || !localMicTrack) {
            try {
                localStream = await navigator.mediaDevices.getUserMedia({
                    audio: {
                        echoCancellation: false,
                        noiseSuppression: false,
                        autoGainControl: false
                    },
                    video: false
                });

                localMicTrack = localStream.getAudioTracks()[0];
                localMicTrack.enabled = true;

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

        if (localMicTrack) localMicTrack.enabled = true;

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

        notifyTalkState(true);
    }

    async function stopTalk() {
        if (!isTalking) return;
        isTalking = false;

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
