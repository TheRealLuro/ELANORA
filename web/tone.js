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

// Swell: the same rate delivered as a continuous tone that only breathes in
// volume. At 35% depth it never reaches silence, so the ear gets no onset to
// latch onto -- which separates entrainment to a periodic amplitude change
// from a startle response to a sound starting.
const SWELL_DEPTH = 0.35;

// Above this the frequency is delivered as the tone itself; below it, as a
// pulse rate gating an audible carrier.
//
// The split is physical. Hearing starts near 20 Hz, so 0.5 Hz cannot be played
// as a tone at all -- it only exists as a rhythm. Above 20 Hz the frequency can
// be the pitch directly, and gating becomes the awkward option instead: a 1 kHz
// gate rate on a 440 Hz carrier is not a rhythm, it is noise.
export const AUDIBLE_CROSSOVER_HZ = 20;

export function deliveryOf(hz) {
  return hz >= AUDIBLE_CROSSOVER_HZ ? "pitch" : "rate";
}

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
    sweepTo = 0,
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

  // Pitch delivery: the stimulus frequency becomes the carrier and the gate
  // stays open, so what is heard is a steady tone at that frequency.
  const pitchMode = condition === "stim" && !rates && hz >= AUDIBLE_CROSSOVER_HZ
                    && sweepTo <= 0;
  const carrier0 = pitchMode ? hz : carrierHz;

  const layers = (rates && rates.length) ? rates : [hz];
  const gatePhase = layers.map(() => 0);

  // Frequency sweep -- a VERIFICATION AID, never a stimulus condition. No
  // recorded round sweeps; every round holds one rate for its full 30 s. This
  // exists so the operator can hear the whole 0.5-45 Hz range in one go and
  // confirm the rate really is changing, which a fixed-rate test tone cannot
  // show. Logarithmic, matching the protocol's own half-octave spacing, so the
  // sweep spends equal time per octave rather than racing through the bottom.
  const sweeping = sweepTo > 0 && sweepTo !== hz && !(rates && rates.length);
  const rateAt = (t) => hz * Math.pow(sweepTo / hz, Math.min(1, t / seconds));

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
  const normalisingGain = (rateList, periods = 8) => {
    if (pitchMode || condition !== "stim" || envelope !== "gated") return 1;
    const positive = rateList.filter((r) => r > 0);
    if (!positive.length) return 1;
    const slowest = Math.min(...positive);

    const steps = Math.round(sr * Math.max(1, periods / slowest));
    const ph = rateList.map(() => 0);
    let e = 0;
    let acc = 0;
    for (let i = 0; i < steps; i++) {
      let sum = 0;
      for (let l = 0; l < rateList.length; l++) {
        ph[l] += rateList[l] * dt;
        if (ph[l] >= 1) ph[l] -= 1;
        sum += ph[l] < clampedDuty ? 1 : 0;
      }
      e += (sum / rateList.length - e) * rampCoeff;
      acc += e * e;
    }
    const measured = Math.sqrt(acc / steps);
    return measured > 1e-9 ? Math.sqrt(clampedDuty) / measured : 1;
  };
  const envGain = normalisingGain(layers);

  // A sweep passes through every rate, and the loudness correction differs at
  // each one -- so a single gain would make the top of the sweep quieter than
  // the bottom, which is the exact confound the correction exists to remove.
  // Sampled at a few log-spaced rates and interpolated; four periods rather
  // than eight keeps a button press responsive at the slow end.
  const sweepTable = [];
  if (sweeping) {
    for (let k = 0; k <= 8; k++) {
      const r = hz * Math.pow(sweepTo / hz, k / 8);
      sweepTable.push(normalisingGain([r], 4));
    }
  }
  const sweepGain = (t) => {
    const u = Math.min(1, Math.max(0, t / seconds)) * 8;
    const i0 = Math.min(7, Math.floor(u));
    return sweepTable[i0] + (sweepTable[i0 + 1] - sweepTable[i0]) * (u - i0);
  };

  let env = 0;
  let carrier = 0;
  let jT = 0;
  let jOn = true;
  let jNext = nextGap();
  const jWidth = clampedDuty / jitterMeanHz;
  const carrierInc = TAU * carrier0 / sr;

  for (let i = 0; i < n; i++) {
    let target = 0;

    if (pitchMode) {
      // A steady tone at the stimulus frequency, scaled like the tone control
      // because it is the same shape of sound.
      target = CONTINUOUS_SCALE;
    } else if (condition === "control_tone") {
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
      // One layer, re-rated per sample. Phase keeps accumulating across the
      // change, so the sweep glides rather than clicking at each step.
      if (sweeping) layers[0] = rateAt(i * dt);
      let sum = 0;
      for (let l = 0; l < layers.length; l++) {
        gatePhase[l] += layers[l] * dt;
        if (gatePhase[l] >= 1) gatePhase[l] -= 1;
        if (envelope === "wave") {
          sum += (1 - Math.cos(TAU * gatePhase[l])) * 0.5 * WAVE_SCALE;
        } else if (envelope === "swell") {
          // Oscillates about 1, normalised to the envelope RMS a 50%-duty gate
          // has -- which is sqrt(duty), not 0.5.
          const m = 1 - SWELL_DEPTH * Math.cos(TAU * gatePhase[l]);
          sum += m / Math.sqrt(1 + SWELL_DEPTH * SWELL_DEPTH * 0.5) *
                 Math.sqrt(clampedDuty);
        } else {
          sum += gatePhase[l] < clampedDuty ? 1 : 0;
        }
      }
      target = sum / layers.length;
    }

    // The follower exists to round hard gate edges. A sine envelope has none,
    // and a 4 ms low-pass sits near 40 Hz, so at the top of the frequency set
    // it would measurably shrink the modulation depth -- quietly making a
    // 45 Hz wave round a weaker stimulus than a 4 Hz one.
    if (!pitchMode && envelope !== "gated" && condition === "stim") env = target;
    else env += (target - env) * rampCoeff;

    carrier += carrierInc;
    if (carrier >= TAU) carrier -= TAU;

    out[i] = Math.sin(carrier) * env * amplitude *
             (sweeping ? sweepGain(i * dt) : envGain);
  }
  return buf;
}
