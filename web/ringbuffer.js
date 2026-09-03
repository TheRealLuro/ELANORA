import { unwrap } from "./decode.js";

// One stream's samples on a reconstructed timebase.
//
// The headset supplies no clock, so time comes from the sequence number:
//
//     t = t0 + (seq * perPacket + i) / rate
//
// A missing sequence therefore leaves a hole of exactly the right width, and
// that hole is filled with NaN rather than closed up. Closing it up would slide
// every later sample earlier in time and quietly break the alignment between
// markers and data -- the kind of corruption that produces plausible numbers.
//
// NaN specifically, not zero and not an interpolated value: a bridged gap looks
// like real signal and a zero-filled one draws a hard spike that reads as a
// physiological event.
export class Stream {
  #rate;
  #perPacket;
  #t0 = 0;
  #first = null;
  #last = null;
  #v = [];
  #dropped = 0;
  #cap;

  constructor(rate, perPacket, seconds = 240) {
    this.#rate = rate;
    this.#perPacket = perPacket;
    // A round is 120 s; 240 s of headroom means a slow upload never races the
    // next round's data out of the buffer.
    this.#cap = Math.ceil(rate * seconds);
  }

  anchor(t0) { this.#t0 = t0; }
  get dropped() { return this.#dropped; }
  get length() { return this.#v.length; }

  push(seq, samples) {
    const u = this.#last === null ? seq : unwrap(this.#last, seq);
    if (this.#first === null) {
      this.#first = u;
      this.#last = u - 1;
    }
    // Bluetooth redelivers and reorders. A packet at or behind the high-water
    // mark is a duplicate, and accepting it would shift everything after it.
    if (u <= this.#last) return;

    const missing = u - this.#last - 1;
    for (let k = 0; k < missing * this.#perPacket; k++) this.#v.push(NaN);
    this.#dropped += missing * this.#perPacket;

    for (const x of samples) this.#v.push(x);
    this.#last = u;

    if (this.#v.length > this.#cap) {
      const drop = this.#v.length - this.#cap;
      this.#v.splice(0, drop);
      // first tracks the packet index of v[0], so it has to move with the
      // window or every timestamp after the first trim would be wrong.
      this.#first += drop / this.#perPacket;
    }
  }

  timeAt(i) {
    return this.#t0 + (this.#first * this.#perPacket + i) / this.#rate;
  }

  // Inclusive of t0, exclusive of t1.
  slice(t0, t1) {
    const t = [];
    const v = [];
    if (this.#first === null) return { t, v };
    for (let i = 0; i < this.#v.length; i++) {
      const ts = this.timeAt(i);
      if (ts < t0) continue;
      if (ts >= t1) break;
      t.push(ts);
      v.push(this.#v[i]);
    }
    return { t, v };
  }
}
