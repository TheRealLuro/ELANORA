// Display-only DSP.
//
// Nothing computed here reaches a CSV. Uploads carry raw samples and every
// feature is recomputed by elanora_data from those, exactly as the desktop path
// does. Otherwise band powers would silently depend on which phone ran the
// session, and two subjects' numbers would not be comparable.

const BANDS = [[1, 4], [4, 8], [8, 13], [13, 30], [30, 45]];
export const BAND_NAMES = ["delta", "theta", "alpha", "beta", "gamma"];

// NaN is excluded rather than counted as zero. Counting a dropped packet as
// silence drags RMS toward zero and mislabels a good electrode as bad, which
// would send the operator off reseating a headset that was fine.
function clean(v) {
  const out = [];
  for (const x of v) if (Number.isFinite(x)) out.push(x);
  return out;
}

export function assess(v, sr) {
  const s = clean(v);
  if (s.length < sr / 4) {
    return { quality: "bad", rmsUv: 0, railed: false, flat: true };
  }

  const mean = s.reduce((a, b) => a + b, 0) / s.length;
  let acc = 0;
  let railedCount = 0;
  for (const x of s) {
    const d = x - mean;
    acc += d * d;
    if (Math.abs(x) > 250) railedCount++;
  }
  const rmsUv = Math.sqrt(acc / s.length);
  const flat = rmsUv < 1e-3;
  const railed = railedCount > s.length * 0.05;

  let quality = "bad";
  if (!flat && !railed) {
    if (rmsUv >= 3 && rmsUv <= 50) quality = "good";
    else if (rmsUv > 50 && rmsUv <= 100) quality = "fair";
  }
  return { quality, rmsUv, railed, flat };
}

// Iterative radix-2 FFT, in place. Small enough that a library would be more
// code than the transform.
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len;
    const half = len >> 1;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < half; k++) {
        const c = Math.cos(ang * k);
        const s = Math.sin(ang * k);
        const ur = re[i + k];
        const ui = im[i + k];
        const vr = re[i + k + half] * c - im[i + k + half] * s;
        const vi = re[i + k + half] * s + im[i + k + half] * c;
        re[i + k] = ur + vr;
        im[i + k] = ui + vi;
        re[i + k + half] = ur - vr;
        im[i + k + half] = ui - vi;
      }
    }
  }
}

// Welch: half-overlapping Hann segments, averaged. A single periodogram over
// the whole window would be far noisier, and the monitor is read at a glance.
export function bandPowers(v, sr) {
  const s = clean(v);
  const nfft = 256;
  const abs = [0, 0, 0, 0, 0];
  const zero = { abs, rel: [0, 0, 0, 0, 0], total: 0 };
  if (s.length < nfft) return zero;

  const mean = s.reduce((a, b) => a + b, 0) / s.length;
  const step = nfft / 2;
  const psd = new Float64Array(nfft / 2 + 1);
  let segments = 0;

  for (let off = 0; off + nfft <= s.length; off += step) {
    const re = new Float64Array(nfft);
    const im = new Float64Array(nfft);
    let winPow = 0;
    for (let i = 0; i < nfft; i++) {
      const w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (nfft - 1));
      re[i] = (s[off + i] - mean) * w;
      winPow += w * w;
    }
    fft(re, im);
    for (let k = 0; k <= nfft / 2; k++) {
      psd[k] += (re[k] * re[k] + im[k] * im[k]) / (winPow * sr);
    }
    segments++;
  }
  if (!segments) return zero;

  const df = sr / nfft;
  let total = 0;
  for (let k = 0; k <= nfft / 2; k++) {
    const f = k * df;
    // One-sided: everything but DC and Nyquist counts twice.
    const fold = (k === 0 || k === nfft / 2) ? 1 : 2;
    const p = (psd[k] / segments) * df * fold;
    for (let b = 0; b < 5; b++) {
      if (f >= BANDS[b][0] && f < BANDS[b][1]) abs[b] += p;
    }
    if (f >= 1 && f < 45) total += p;
  }
  const rel = abs.map((x) => (total > 0 ? x / total : 0));
  return { abs, rel, total };
}

// The strongest bin between 1 and 45 Hz. Reported alongside band power because
// a peak at 9.2 Hz and one at 12.8 Hz are both "alpha" and mean different
// things.
export function peakHz(v, sr) {
  const s = clean(v);
  const nfft = 256;
  if (s.length < nfft) return 0;

  const mean = s.reduce((a, b) => a + b, 0) / s.length;
  const re = new Float64Array(nfft);
  const im = new Float64Array(nfft);
  for (let i = 0; i < nfft; i++) {
    const w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (nfft - 1));
    re[i] = (s[s.length - nfft + i] - mean) * w;
  }
  fft(re, im);

  const df = sr / nfft;
  let best = 0;
  let bestK = 0;
  for (let k = 1; k <= nfft / 2; k++) {
    const f = k * df;
    if (f < 1 || f >= 45) continue;
    const p = re[k] * re[k] + im[k] * im[k];
    if (p > best) { best = p; bestK = k; }
  }
  return bestK * df;
}

// ---------------------------------------------------------------------------
// Heart, breathing and motion
// ---------------------------------------------------------------------------
//
// Ported from heart_features.cpp, breath_features.cpp and qc.cpp rather than
// simplified from them. The first version here cut three things -- the
// Butterworth filter, the spectral cross-check, and the confidence terms --
// and what that produced was not a rougher number but a confidently wrong one.
//
// These still never reach a CSV: elanora_data recomputes every stored feature
// from the raw samples. They exist so the operator can see, before committing
// to 36 minutes, that PPG and IMU are producing something physiological. That
// job needs an honest uncertainty far more than it needs a tidy number.

function detrend(v) {
  const s = clean(v);
  if (s.length < 2) return s;
  // PPG rides on a large slow baseline and IMU on gravity; leaving either in
  // place buries the oscillation being looked for.
  const n = s.length;
  let sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (let i = 0; i < n; i++) { sx += i; sy += s[i]; sxx += i * i; sxy += i * s[i]; }
  const d = n * sxx - sx * sx;
  const slope = d !== 0 ? (n * sxy - sx * sy) / d : 0;
  const icpt = (sy - slope * sx) / n;
  return s.map((x, i) => x - (slope * i + icpt));
}

// Direct form II transposed, so one section chains into the next.
function biquad(v, b0, b1, b2, a1, a2) {
  const out = new Array(v.length);
  let z1 = 0, z2 = 0;
  for (let i = 0; i < v.length; i++) {
    const x = v[i];
    const y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    out[i] = y;
  }
  return out;
}

// Second-order Butterworth sections. Two of each gives the 4th order the C++
// uses, which is the difference between a filter that selects a band and one
// that merely leans toward it.
function lowpass2(v, sr, fc) {
  const k = Math.tan(Math.PI * Math.min(fc / sr, 0.49));
  const kk = k * k;
  const n = 1 / (1 + Math.SQRT2 * k + kk);
  return biquad(v, kk * n, 2 * kk * n, kk * n,
                2 * (kk - 1) * n, (1 - Math.SQRT2 * k + kk) * n);
}

function highpass2(v, sr, fc) {
  const k = Math.tan(Math.PI * Math.min(fc / sr, 0.49));
  const kk = k * k;
  const n = 1 / (1 + Math.SQRT2 * k + kk);
  return biquad(v, n, -2 * n, n,
                2 * (kk - 1) * n, (1 - Math.SQRT2 * k + kk) * n);
}

function bandpass4(v, sr, lo, hi) {
  let x = highpass2(v, sr, lo);
  x = highpass2(x, sr, lo);
  x = lowpass2(x, sr, hi);
  x = lowpass2(x, sr, hi);
  return x;
}

function meanOf(v) {
  if (!v.length) return 0;
  let m = 0;
  for (const x of v) m += x;
  return m / v.length;
}

function varianceOf(v) {
  if (v.length < 2) return 0;
  const m = meanOf(v);
  let acc = 0;
  for (const x of v) acc += (x - m) * (x - m);
  return acc / v.length;
}

// Beats per minute from the IR channel, with the quality term that decides
// whether to believe it.
//
// quality is the fraction of inter-beat intervals that were physiologically
// plausible, times a regularity term. A run of movement artifact produces
// plenty of peaks but wildly irregular spacing, and that is what drives the
// number down rather than any single threshold.
export function heartRate(v, sr) {
  const none = { bpm: 0, beats: 0, quality: 0 };
  const s = bandpass4(detrend(v), sr, 0.5, 4.0);
  if (s.length < sr * 8) return none;

  const m = meanOf(s);
  const sd = Math.sqrt(varianceOf(s));
  if (sd <= 0) return none;
  const thr = m + 0.5 * sd;

  const refractory = Math.max(1, Math.round(0.3 * sr));   // 200 BPM ceiling
  const peaks = [];
  let last = -refractory;
  for (let i = 1; i < s.length - 1; i++) {
    if (s[i] <= thr || s[i] < s[i - 1] || s[i] < s[i + 1]) continue;
    if (i - last < refractory) continue;
    peaks.push(i);
    last = i;
  }
  if (peaks.length < 4) return { bpm: 0, beats: peaks.length, quality: 0 };

  // Intervals outside 0.25-2.0 s are not heartbeats. Averaging them in is how
  // a movement artifact becomes a plausible-looking BPM.
  const all = [];
  const kept = [];
  for (let i = 1; i < peaks.length; i++) {
    const dt = (peaks[i] - peaks[i - 1]) / sr;
    all.push(dt);
    if (dt >= 0.25 && dt <= 2.0) kept.push(dt);
  }
  if (kept.length < 3) return { bpm: 0, beats: peaks.length, quality: 0 };

  let mIbi = meanOf(kept);
  const accepted = kept.length / all.length;
  // Coefficient of variation: a real pulse is regular, an artifact train is not.
  const cv = Math.sqrt(varianceOf(kept)) / Math.max(mIbi, 1e-9);
  let regularity = Math.max(0, 1 - cv / 0.35);

  let bpm = 60 / mIbi;

  // Octave check against an independent spectral estimate.
  //
  // Real PPG has a dicrotic notch -- a second smaller peak within each beat --
  // and a threshold detector happily counts it, reporting exactly twice the
  // true rate. That error is REGULAR, so the coefficient of variation above
  // rates it as excellent quality: a confidently doubled heart rate. Counting
  // peaks cannot detect this about itself, which is why a second method that
  // does not count peaks is needed.
  const spectral = dominantHz(s, sr, 0.5, 4.0) * 60;
  if (spectral > 0) {
    if (Math.abs(bpm - 2 * spectral) < Math.abs(bpm - spectral)) {
      // Double-counting: the periodogram sees the fundamental, the detector
      // saw the notch as well.
      bpm = spectral;
      mIbi = 60 / bpm;
    } else if (Math.abs(bpm - spectral / 2) < Math.abs(bpm - spectral)) {
      // Half-counting: beats were missed, so the spectral peak is a harmonic.
      bpm = spectral / 2;
      mIbi = 60 / bpm;
    }
    // Whatever the correction, disagreement is still uncertainty.
    const hi = Math.max(bpm, spectral, 1e-9);
    regularity *= Math.max(0.2, 1 - Math.abs(bpm - spectral) / hi);
  }

  return {
    bpm,
    spectral,
    beats: peaks.length,
    quality: Math.max(0, Math.min(1, accepted * regularity)),
  };
}

// Dominant frequency of a Hann-windowed periodogram over the whole record.
//
// Not Welch: averaging sub-segments would throw away exactly the frequency
// resolution that makes a 30 s breathing window worth having in the first
// place.
function dominantHz(v, sr, lo, hi) {
  let n = 1;
  while (n * 2 <= v.length) n *= 2;
  if (n < 64) return 0;

  const re = new Float64Array(n);
  const im = new Float64Array(n);
  const m = meanOf(v);
  for (let i = 0; i < n; i++) {
    const w = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1));
    re[i] = (v[i] - m) * w;
  }
  fft(re, im);

  const df = sr / n;
  let best = 0;
  let bestK = 0;
  for (let k = 1; k <= n / 2; k++) {
    const f = k * df;
    if (f < lo || f > hi) continue;
    const p = re[k] * re[k] + im[k] * im[k];
    if (p > best) { best = p; bestK = k; }
  }
  return bestK * df;
}

// Breaths per minute from head motion, with two independent estimators.
//
// Breathing drives small slow head movement. The axis carrying the most
// in-band energy wins, and its name is reported because which axis carries the
// signal depends on how the headset happens to sit.
//
// confidence multiplies three terms, each in [0,1]: enough cycles to measure,
// agreement between the zero-crossing and spectral estimates, and in-band
// energy against the total. Agreement is the important one -- two methods that
// disagree are the clearest possible signal that neither should be trusted,
// and a single estimator cannot tell you that at all.
export function breathRate(axes, sr) {
  const names = ["ax", "ay", "az", "gx", "gy", "gz"];
  const none = { brpm: 0, spectral: 0, axis: "", cycles: 0, confidence: 0 };

  let best = -1;
  let bestVar = 0;
  let bestFiltered = null;
  let bestRaw = null;
  for (let a = 0; a < axes.length; a++) {
    const raw = detrend(axes[a]);
    if (raw.length < sr * 20) continue;
    const f = bandpass4(raw, sr, 0.1, 0.5);
    const vv = varianceOf(f);
    if (vv > bestVar) { bestVar = vv; best = a; bestFiltered = f; bestRaw = raw; }
  }
  if (best < 0 || !bestFiltered) return none;

  // Positive-going zero crossings: a cycle count that ignores amplitude.
  const cross = [];
  for (let i = 1; i < bestFiltered.length; i++) {
    if (bestFiltered[i - 1] <= 0 && bestFiltered[i] > 0) cross.push(i);
  }
  const cycles = Math.max(0, cross.length - 1);
  let brpm = 0;
  if (cycles >= 1) {
    const span = (cross[cross.length - 1] - cross[0]) / sr;
    if (span > 0) brpm = (cycles / span) * 60;
  }

  const spectral = dominantHz(bestFiltered, sr, 0.1, 0.5) * 60;

  const cCycles = Math.min(1, cycles / 6);
  let cAgree = 0;
  if (brpm > 0 && spectral > 0) {
    const hi = Math.max(brpm, spectral);
    cAgree = Math.max(0, 1 - Math.abs(brpm - spectral) / hi);
  }
  const totalVar = varianceOf(bestRaw);
  const cBand = totalVar > 0 ? Math.min(1, (bestVar / totalVar) * 3) : 0;
  const confidence = Math.max(0, Math.min(1, cCycles * cAgree * cBand));

  return { brpm, spectral, axis: names[best], cycles, confidence };
}

// Summed variance of the six axes band-passed 1-10 Hz.
//
// Deliberately above the respiratory band so normal breathing cannot trip it.
// This is the substitute for the EMG channel the Muse does not have: jaw
// clench and movement are inferred here and used to discount suspect gamma.
export function motionEnergy(axes, sr) {
  let total = 0;
  for (const axis of axes) {
    const raw = detrend(axis);
    if (raw.length < sr) continue;
    total += varianceOf(bandpass4(raw, sr, 1.0, 10.0));
  }
  return Math.sqrt(total);
}
