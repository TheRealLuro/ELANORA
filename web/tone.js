// Isochronic stimulus, rendered in the browser.
//
// A sample-by-sample port of collector/src/tone.cpp. The whole round is
// rendered into one AudioBuffer and started with source.start(t), rather than
// streamed through gain automation: start() is sample-accurate, so stimulus
// onset lands exactly where the marker says it did. Scheduling gate edges with
// setTimeout would put tens of milliseconds of jitter on the one timing this
// experiment most depends on.
//
// The port has to stay faithful, because RMS matching across the three
// conditions IS the control. If the jitter control is quieter than the stimulus
// rounds, loudness becomes the difference the control exists to rule out.
// selftest.html asserts the same properties test_tone.cpp does.

const TAU = 6.283185307179586;

// A 50%-duty gated sine of amplitude A has RMS A/2; a continuous sine of the
// same amplitude has RMS A/sqrt(2). Without this factor the tone control is
// simply louder than every stimulus round.
const CONTINUOUS_SCALE = 0.70710678118654752;

// Sine-envelope rounds are scaled so they match a gated round in loudness.
// A 50%-duty gated sine has mean square A^2/4; a sine-modulated one with
// envelope (1-cos)/2 has 3A^2/16. Equalising the two gives 2/sqrt(3).
const WAVE_SCALE = 1.1547005383792515;

// xorshift64, matching ToneGenerator::next_jitter_gap, so a recorded jitter
// round can be regenerated exactly from its seed when reviewing a session.
function xorshift(state) {
  let s = state;
  s ^= (s << 13n) & 0xffffffffffffffffn;
  s ^= s >> 7n;
  s ^= (s << 17n) & 0xffffffffffffffffn;
  return s & 0xffffffffffffffffn;
}

export function renderStimulus(ctx, opts) {
  const {
    condition = "stim",
    hz = 10,
    rates = null,
    jitterMeanHz = 10,
    seed = 1,
    carrierHz = 440,
    duty = 0.5,
    seconds = 30,
    amplitude = 0.5,
    envelope = "gated",
  } = opts;

  const sr = ctx.sampleRate;
  const n = Math.max(1, Math.round(seconds * sr));
  const buf = ctx.createBuffer(1, n, sr);
  const out = buf.getChannelData(0);

  // ~4 ms one-pole ramp: long enough to remove the click, short enough that the
  // pulse edge is still sharp at 45 Hz, where a full period is only 22 ms.
  const rampCoeff = 1 - Math.exp(-1 / (0.004 * sr));
  const dt = 1 / sr;
  const clampedDuty = Math.min(0.95, Math.max(0.05, duty));

  const layers = (rates && rates.length) ? rates : [hz];
  const gatePhase = layers.map(() => 0);

  let rng = BigInt(seed || 1);
  const nextGap = () => {
    rng = xorshift(rng);
    const u = Number(rng % 1000000n) / 1000000;
    // This is the OFF time, not the whole period. Returning a full period here
    // made the real period gap + width, so a 10 Hz jitter control ran at 6.7
    // pulses per second and no longer matched the stimulus rounds on pulse
    // count -- the one thing the control has to hold constant.
    return (1 / jitterMeanHz) * (1 - clampedDuty) * (0.55 + 0.9 * u);
  };

  // Per-round gain that makes every gated rate equally loud.
  //
  // The 4 ms follower that rounds gate edges also removes energy, and removes
  // more of it the more edges there are: across the protocol's own frequency
  // set, RMS fell from 0.2495 at 0.5 Hz to 0.2065 at 45 Hz, so the top of the
  // sweep was 17% quieter than the bottom. Loudness covarying with stimulus
  // frequency is the confound the RMS matching exists to eliminate.
  //
  // Measured over whole periods of the SLOWEST layer: a fixed one-second
  // window puts 0.5 Hz, whose period is two seconds, at 0.177 instead of 0.250.
  const normalisingGain = () => {
    if (condition !== "stim" || envelope !== "gated") return 1;
    const positive = layers.filter((r) => r > 0);
    if (!positive.length) return 1;
    const slowest = Math.min(...positive);

    const steps = Math.round(sr * Math.max(1, 8 / slowest));
    const ph = layers.map(() => 0);
    let e = 0;
    let acc = 0;
    for (let i = 0; i < steps; i++) {
      let sum = 0;
      for (let l = 0; l < layers.length; l++) {
        ph[l] += layers[l] * dt;
        if (ph[l] >= 1) ph[l] -= 1;
        sum += ph[l] < clampedDuty ? 1 : 0;
      }
      e += (sum / layers.length - e) * rampCoeff;
      acc += e * e;
    }
    const measured = Math.sqrt(acc / steps);
    return measured > 1e-9 ? Math.sqrt(clampedDuty) / measured : 1;
  };
  const envGain = normalisingGain();

  let env = 0;
  let carrier = 0;
  let jT = 0;
  let jOn = true;
  let jNext = nextGap();
  const jWidth = clampedDuty / jitterMeanHz;
  const carrierInc = TAU * carrierHz / sr;

  for (let i = 0; i < n; i++) {
    let target = 0;

    if (condition === "control_tone") {
      target = CONTINUOUS_SCALE;
    } else if (condition === "control_jitter") {
      jT += dt;
      if (!jOn && jT >= jNext) {
        jOn = true;
        jT = 0;
      } else if (jOn && jT >= jWidth) {
        jOn = false;
        jT = 0;
        jNext = nextGap();
      }
      target = jOn ? 1 : 0;
    } else {
      // Sum the envelopes and divide by the layer count, so stacking can never
      // clip and adding a layer never raises the level.
      let sum = 0;
      for (let l = 0; l < layers.length; l++) {
        gatePhase[l] += layers[l] * dt;
        if (gatePhase[l] >= 1) gatePhase[l] -= 1;
        sum += envelope === "wave"
          ? (1 - Math.cos(TAU * gatePhase[l])) * 0.5 * WAVE_SCALE
          : (gatePhase[l] < clampedDuty ? 1 : 0);
      }
      target = sum / layers.length;
    }

    // The follower exists to round hard gate edges. A sine envelope has none,
    // and a 4 ms low-pass sits near 40 Hz, so at the top of the frequency set
    // it would measurably shrink the modulation depth -- quietly making a
    // 45 Hz wave round a weaker stimulus than a 4 Hz one.
    if (envelope === "wave" && condition === "stim") env = target;
    else env += (target - env) * rampCoeff;

    carrier += carrierInc;
    if (carrier >= TAU) carrier -= TAU;

    out[i] = Math.sin(carrier) * env * amplitude * envGain;
  }
  return buf;
}
