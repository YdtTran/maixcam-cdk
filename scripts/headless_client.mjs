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
  async eval(expression) {
    await this.ready;
    const id = ++this.id;
    const p = new Promise((res, rej) => this.pending.set(id, { res, rej }));
    this.ws.send(JSON.stringify({
      id,
      method: 'Runtime.evaluate',
      params: { expression, awaitPromise: true, returnByValue: true },
    }));
    const r = await p;
    return r?.result?.value;
  }
  close() {
    try { this.ws.close(); } catch {}
  }
}

try {
  const wsUrl = await getWsUrl();
  const cdp = new CDP(wsUrl);

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

  console.log(`STREAMING duration=${durationSec}s talk=${enableTalk}`);
  await sleep(durationSec * 1000);
  cdp.close();
} finally {
  cleanup();
}
