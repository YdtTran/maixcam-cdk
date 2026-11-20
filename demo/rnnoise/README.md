# RNNoise microphone demo

From the repository root, run `python3 -m http.server 8765 --bind 127.0.0.1`, then open <http://localhost:8765/demo/rnnoise/> on the same computer. Use headphones and allow microphone access. Switch filtering on and off to compare. Nothing is sent to MaixCAM or uploaded.

RNNoise adapts its suppression to the incoming audio. This demo does not implement a separate adaptive voice gate or NoiseTorch's voice activation threshold. It requires a browser supporting AudioWorklet and a 48 kHz AudioContext.

`rnnoise-sync.js` is Jitsi's prebuilt RNNoise WebAssembly module from https://github.com/jitsi/rnnoise-wasm (master, retrieved 2026-11-18); see the accompanying license files.
