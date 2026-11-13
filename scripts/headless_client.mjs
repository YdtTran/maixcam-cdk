#!/usr/bin/env node
/**
 * Reproducible headless Chromium WebRTC client via CDP (Node 22 built-ins only).
 * Usage: node scripts/headless_client.mjs [--url https://10.172.16.1:8000/] [--duration 15] [--talk] [--listen]
 */
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';

const args = process.argv.slice(2);
let url = 'https://10.172.16.1:8000/';
let durationSec = 15;
let enableTalk = false;
let enableListen = false;
let cdpPort = 9229;

for (let i = 0; i < args.length; i++) {
  if (args[i] === '--url' && args[i + 1]) url = args[++i];
  else if (args[i] === '--duration' && args[i + 1]) durationSec = Number(args[++i]);
  else if (args[i] === '--talk') enableTalk = true;
  else if (args[i] === '--listen') enableListen = true;
  else if (args[i] === '--port' && args[i + 1]) cdpPort = Number(args[++i]);
}

const chromeBin = join(
  homedir(),
  '.cache/ms-playwright/chromium_headless_shell-1234/chrome-headless-shell-linux64/chrome-headless-shell'
);
if (!existsSync(chromeBin)) {
  console.error('Missing chrome-headless-shell at', chromeBin);
  process.exit(1);
}

const proc = spawn(
  chromeBin,
  [
    `--remote-debugging-port=${cdpPort}`,
    '--ignore-certificate-errors',
    '--use-fake-ui-for-media-stream',
    '--use-fake-device-for-media-stream',
    '--autoplay-policy=no-user-gesture-required',
    '--no-sandbox',
    '--disable-gpu',
    '--headless=new',
    url,
  ],
  { stdio: ['ignore', 'ignore', 'ignore'] }
);

const cleanup = () => {
  try { proc.kill('SIGKILL'); } catch {}
};
process.on('exit', cleanup);
process.on('SIGINT', () => { cleanup(); process.exit(130); });
process.on('SIGTERM', () => { cleanup(); process.exit(143); });

async function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function getWsUrl() {
  for (let i = 0; i < 40; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${cdpPort}/json`);
      const pages = await res.json();
      const page = pages.find((p) => p.type === 'page' && p.webSocketDebuggerUrl);
      if (page) return page.webSocketDebuggerUrl;
    } catch {}
    await sleep(150);
  }
  throw new Error('Timed out waiting for CDP');
}

class CDP {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl);
    this.id = 0;
    this.pending = new Map();
    this.ready = new Promise((res, rej) => {
      this.ws.onopen = res;
      this.ws.onerror = rej;
    });
    this.ws.onmessage = (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id && this.pending.has(msg.id)) {
        const { res, rej } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        if (msg.error) rej(new Error(msg.error.message));
        else res(msg.result);
      }
    };
  }
  async call(method, params = {}) {
    await this.ready;
    const id = ++this.id;
    const p = new Promise((res, rej) => this.pending.set(id, { res, rej }));
    this.ws.send(JSON.stringify({ id, method, params }));
    return p;
  }
  async eval(expression) {
    const r = await this.call('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    return r?.result?.value;
  }
  close() {
    try { this.ws.close(); } catch {}
  }
}

try {
  const wsUrl = await getWsUrl();
  const cdp = new CDP(wsUrl);

  // Hook RTCPeerConnection to expose stats without modifying client.js
  await cdp.call('Page.enable');
  await cdp.call('Page.addScriptToEvaluateOnNewDocument', {
    source: `(() => {
      const OrigPC = window.RTCPeerConnection;
      if (!OrigPC || window.__g4c4PcHooked) return;
      window.__g4c4PcHooked = true;
      window.__g4c4Peer = null;
      window.RTCPeerConnection = function(...args) {
        const pc = new OrigPC(...args);
        window.__g4c4Peer = pc;
        return pc;
      };
      window.RTCPeerConnection.prototype = OrigPC.prototype;
    })();`
  });
  await cdp.call('Page.reload', { ignoreCache: true });

  // Wait for WebRTC PeerConnection to connect and video frames to arrive
  let connected = false;
  for (let i = 0; i < 40; i++) {
    const st = await cdp.eval(`(() => {
      const v = document.querySelector('video');
      const talk = document.getElementById('talk-btn');
      return { ready: !!(v && v.readyState >= 2), w: v?.videoWidth || 0, h: v?.videoHeight || 0, hasTalk: !!talk };
    })()`);
    if (st && st.ready && st.w > 0) {
      connected = true;
      console.log(`CONNECTED video=${st.w}x${st.h}`);
      break;
    }
    await sleep(250);
  }
  if (!connected) {
    console.error('WARNING: video not playing after 10s');
  }

  if (enableListen) {
    await cdp.eval(`document.getElementById('listen-btn')?.click()`);
    console.log('LISTEN_CLICKED');
  }
  if (enableTalk) {
    await sleep(500);
    await cdp.eval(`document.getElementById('talk-btn')?.click()`);
    console.log('TALK_CLICKED');
  }

  async function readStats() {
    return await cdp.eval(`(async () => {
      const out = { ts: performance.now(), vq: null, video: null, audio_in: null, audio_out: null };
      const v = document.querySelector('video');
      if (v && v.getVideoPlaybackQuality) {
        const q = v.getVideoPlaybackQuality();
        out.vq = { total: q.totalVideoFrames, dropped: q.droppedVideoFrames };
      }
      const pc = window.__g4c4Peer;
      if (pc) {
        const stats = await pc.getStats();
        stats.forEach((r) => {
          if (r.type === 'inbound-rtp' && r.kind === 'video') {
            out.video = {
              rx_frames: r.framesReceived || 0,
              dec_frames: r.framesDecoded || 0,
              drop_frames: r.framesDropped || 0,
              fps: r.framesPerSecond || 0,
              pkts_rx: r.packetsReceived || 0,
              pkts_lost: r.packetsLost || 0,
              jitter: r.jitter || 0,
              jb_delay: r.jitterBufferDelay || 0,
              jb_emitted: r.jitterBufferEmittedCount || 0,
              decode_time: r.totalDecodeTime || 0,
              bytes: r.bytesReceived || 0,
            };
          } else if (r.type === 'inbound-rtp' && r.kind === 'audio') {
            out.audio_in = {
              pkts_rx: r.packetsReceived || 0,
              pkts_lost: r.packetsLost || 0,
              jitter: r.jitter || 0,
              jb_delay: r.jitterBufferDelay || 0,
              jb_emitted: r.jitterBufferEmittedCount || 0,
              concealed: r.concealedSamples || 0,
            };
          } else if (r.type === 'outbound-rtp' && r.kind === 'audio') {
            out.audio_out = {
              pkts_sent: r.packetsSent || 0,
              bytes: r.bytesSent || 0,
            };
          }
        });
      }
      return out;
    })()`);
  }

  console.log(`STREAMING duration=${durationSec}s talk=${enableTalk}`);
  const st0 = await readStats();
  await sleep(durationSec * 1000);
  const st1 = await readStats();
  if (st0 && st1 && st0.video && st1.video) {
    const dt = (st1.ts - st0.ts) / 1000.0;
    const v0 = st0.video, v1 = st1.video;
    const rxFps = (v1.rx_frames - v0.rx_frames) / dt;
    const decFps = (v1.dec_frames - v0.dec_frames) / dt;
    const dropF = v1.drop_frames - v0.drop_frames;
    const dispFps = (st1.vq && st0.vq) ? (st1.vq.total - st0.vq.total) / dt : 0;
    const dispDrop = (st1.vq && st0.vq) ? (st1.vq.dropped - st0.vq.dropped) : 0;
    const jbMs = (v1.jb_emitted > v0.jb_emitted)
      ? 1000.0 * (v1.jb_delay - v0.jb_delay) / (v1.jb_emitted - v0.jb_emitted) : 0;
    const decMs = (v1.dec_frames > v0.dec_frames)
      ? 1000.0 * (v1.decode_time - v0.decode_time) / (v1.dec_frames - v0.dec_frames) : 0;
    console.log(
      `BROWSER_VIDEO dt=${dt.toFixed(2)}s rx_fps=${rxFps.toFixed(2)} dec_fps=${decFps.toFixed(2)} ` +
      `disp_fps=${dispFps.toFixed(2)} rtp_drop=${dropF} disp_drop=${dispDrop} ` +
      `pkts_rx=${v1.pkts_rx - v0.pkts_rx} pkts_lost=${v1.pkts_lost - v0.pkts_lost} ` +
      `jitter_ms=${(v1.jitter * 1000).toFixed(2)} jb_ms=${jbMs.toFixed(2)} dec_ms=${decMs.toFixed(2)}`
    );
  }
  if (st0 && st1 && st0.audio_in && st1.audio_in) {
    const a0 = st0.audio_in, a1 = st1.audio_in;
    const jbMs = (a1.jb_emitted > a0.jb_emitted)
      ? 1000.0 * (a1.jb_delay - a0.jb_delay) / (a1.jb_emitted - a0.jb_emitted) : 0;
    console.log(
      `BROWSER_AUDIO_IN pkts_rx=${a1.pkts_rx - a0.pkts_rx} pkts_lost=${a1.pkts_lost - a0.pkts_lost} ` +
      `jitter_ms=${(a1.jitter * 1000).toFixed(2)} jb_ms=${jbMs.toFixed(2)} concealed=${a1.concealed - a0.concealed}`
    );
  }
  cdp.close();
} finally {
  cleanup();
}
