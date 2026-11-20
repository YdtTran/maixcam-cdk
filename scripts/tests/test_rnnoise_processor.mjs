import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';

const moduleSource = fs.readFileSync('demo/rnnoise/rnnoise-sync.js', 'utf8');
const moduleUrl = 'data:text/javascript;base64,' + Buffer.from(moduleSource).toString('base64');
const { default: createModule } = await import(moduleUrl);
const rnnoise = await createModule();
let Processor;
const reports = [];
const context = vm.createContext({
    rnnoise, Float32Array,
    AudioWorkletProcessor: class { constructor() { this.port = { postMessage: value => reports.push(value) }; } },
    registerProcessor: (_, processor) => { Processor = processor; },
});
const source = fs.readFileSync('demo/rnnoise/processor.js', 'utf8')
    .replace("import createModule from './rnnoise-sync.js';", '')
    .replace('const rnnoise = await createModule();', '');
vm.runInContext(source, context);
const processor = new Processor();
assert.equal(typeof processor.port.onmessage, 'function', 'filter accepts manual strength');
processor.port.onmessage({ data: { mix: 0 } });
assert.equal(processor.mix, 0);
processor.port.onmessage({ data: { mix: 1 } });
assert.equal(processor.mix, 1);
let nonzero = false;
for (let block = 0; block < 60; block++) {
    const input = Float32Array.from({ length: 128 }, (_, i) => 0.2 * Math.sin((block * 128 + i) * 2 * Math.PI * 440 / 48000));
    const output = new Float32Array(128);
    assert.equal(processor.process([[input]], [[output]]), true);
    assert(output.every(Number.isFinite));
    if (output.some(sample => Math.abs(sample) > 0.00001)) nonzero = true;
}
assert(nonzero, 'RNNoise produces non-silent output for a tone');
assert(reports.some(report => report.input > 0 && report.output > 0), 'reports microphone and filtered levels');
console.log('RNNoise worklet processes audio and reports levels: PASS');
