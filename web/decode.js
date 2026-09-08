// Muse 2 packet decode.
//
// Pure functions over DataView, so every one is testable without a headset.
//
// The headset sends a 16-bit sequence number per packet and no clock of any
// kind, so nothing here invents a timestamp. Turning sequences into a timebase
// is ringbuffer.js's job, and keeping the two apart is what lets the decode be
// checked against captured bytes.

const EEG_SCALE = 125 / 256;   // 0.48828125 uV per count
const EEG_ZERO = 0x800;        // 12-bit midpoint is zero volts

export const ACCEL_SCALE = 1 / 16384;      // g per count
export const GYRO_SCALE = 0.0074768;       // deg/s per count

export const EEG_PER_PACKET = 12;
export const PPG_PER_PACKET = 6;
export const IMU_PER_PACKET = 3;

// Twelve 12-bit samples packed into 18 bytes, two samples per three bytes,
// high nibble first.
export function decodeEeg(dv) {
  const seq = dv.getUint16(0, false);
  const samples = new Array(EEG_PER_PACKET);
  for (let i = 0; i < EEG_PER_PACKET; i++) {
    const bit = i * 12;
    const byte = 2 + (bit >> 3);
    const raw = (bit & 7) === 0
      ? (dv.getUint8(byte) << 4) | (dv.getUint8(byte + 1) >> 4)
      : ((dv.getUint8(byte) & 0x0f) << 8) | dv.getUint8(byte + 1);
    samples[i] = (raw - EEG_ZERO) * EEG_SCALE;
  }
  return { seq, samples };
}

// Six 24-bit big-endian counts. Left raw on purpose: the PPG scale is not
// physically meaningful, and every heart feature is computed downstream from
// the raw trace by elanora_data.
export function decodePpg(dv) {
  const seq = dv.getUint16(0, false);
  const samples = new Array(PPG_PER_PACKET);
  for (let i = 0; i < PPG_PER_PACKET; i++) {
    const b = 2 + i * 3;
    samples[i] = (dv.getUint8(b) << 16) | (dv.getUint8(b + 1) << 8) | dv.getUint8(b + 2);
  }
  return { seq, samples };
}

// Three triplets of 16-bit signed, scaled by the caller because accel and gyro
// share this layout but not their units.
export function decodeImu(dv, scale) {
  const seq = dv.getUint16(0, false);
  const samples = [];
  for (let i = 0; i < IMU_PER_PACKET; i++) {
    const b = 2 + i * 6;
    samples.push([
      dv.getInt16(b, false) * scale,
      dv.getInt16(b + 2, false) * scale,
      dv.getInt16(b + 4, false) * scale,
    ]);
  }
  return { seq, samples };
}

// The counter is 16 bits, so at 256 Hz it wraps roughly every 51 minutes --
// inside a single 36-minute session plus setup. Left unwrapped, timestamps
// would jump backwards partway through a recording, and every sample after the
// wrap would land before the ones preceding it.
export function unwrap(prev, seq) {
  const base = prev - ((prev % 65536) + 65536) % 65536;
  let out = base + seq;
  if (out < prev - 32768) out += 65536;
  if (out > prev + 32768) out -= 65536;
  return out;
}

// Battery and temperature.
//
// The headset sends this unprompted roughly once a second and it was being
// discarded. It matters over a 36-minute session: a headset that starts at
// 100% and ends at 20% has a changing supply rail, and knowing that is the
// difference between explaining a drift and guessing at it.
//
// 16-bit big-endian fields after the sequence: battery in hundredths of a
// percent, fuel gauge in 2.2 uV units, ADC millivolts, temperature in Celsius.
export function decodeTelemetry(dv) {
  return {
    seq: dv.getUint16(0, false),
    batteryPct: dv.getUint16(2, false) / 512,
    fuelGaugeUv: dv.getUint16(4, false) * 2.2,
    adcMv: dv.getUint16(6, false),
    temperatureC: dv.getUint16(8, false),
  };
}
