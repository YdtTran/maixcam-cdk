import createModule from './rnnoise-sync.js';

const rnnoise = await createModule();

class Denoiser extends AudioWorkletProcessor {
    constructor() {
        super();
        this.state = rnnoise._rnnoise_create();
        this.input = rnnoise._malloc(480 * 4);
        this.output = rnnoise._malloc(480 * 4);
        this.frame = new Float32Array(480);
        this.filtered = new Float32Array(480);
        this.position = 0;
        this.inputEnergy = 0;
        this.outputEnergy = 0;
        this.frames = 0;
        this.mix = 1;
        this.port.onmessage = ({ data }) => {
            if (typeof data.mix === 'number' && Number.isFinite(data.mix)) this.mix = Math.max(0, Math.min(1, data.mix));
        };
    }

    process(inputs, outputs) {
        const input = inputs[0][0];
        const output = outputs[0][0];
        if (!input || !output) return true;
        for (let i = 0; i < output.length; i++) {
            output[i] = this.filtered[this.position];
            this.frame[this.position++] = input[i] * 32768;
            if (this.position === 480) {
                rnnoise.HEAPF32.set(this.frame, this.input >> 2);
                rnnoise._rnnoise_process_frame(this.state, this.output, this.input);
                const result = rnnoise.HEAPF32.subarray(this.output >> 2, (this.output >> 2) + 480);
                for (let j = 0; j < 480; j++) {
                    this.filtered[j] = (result[j] * this.mix + this.frame[j] * (1 - this.mix)) / 32768;
                    this.inputEnergy += (this.frame[j] / 32768) ** 2;
                    this.outputEnergy += this.filtered[j] ** 2;
                }
                if (++this.frames === 10) {
                    this.port.postMessage({ input: Math.sqrt(this.inputEnergy / 4800), output: Math.sqrt(this.outputEnergy / 4800) });
                    this.inputEnergy = this.outputEnergy = this.frames = 0;
                }
                this.position = 0;
            }
        }
        return true;
    }
}
registerProcessor('rnnoise', Denoiser);
