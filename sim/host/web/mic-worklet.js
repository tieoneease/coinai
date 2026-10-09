// Downsample the browser microphone to 16 kHz mono s16 in 20 ms chunks (box-filter decimation).
class MicDown extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ratio = sampleRate / 16000;
    this.pos = 0; this.acc = 0; this.cnt = 0;
    this.buf = new Int16Array(320); this.n = 0;
  }
  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (!ch) return true;
    for (let i = 0; i < ch.length; i++) {
      this.acc += ch[i]; this.cnt++; this.pos += 1;
      if (this.pos >= this.ratio) {
        this.pos -= this.ratio;
        const v = this.acc / this.cnt; this.acc = 0; this.cnt = 0;
        this.buf[this.n++] = Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
        if (this.n === this.buf.length) { this.port.postMessage(this.buf.slice(0)); this.n = 0; }
      }
    }
    return true;
  }
}
registerProcessor('mic-down', MicDown);
