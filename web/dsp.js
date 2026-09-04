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
// Heart, breathing and motion -- display only
// ---------------------------------------------------------------------------
//
// Simplified counterparts to heart_features.cpp, breath_features.cpp and
// qc.cpp. They exist so the operator can see that PPG and IMU are producing
// something physiological before committing to a 36-minute session; every
// number that reaches a dataset is still recomputed by elanora_data from the
// raw samples.

function detrend(v) {
  const s = clean(v);
  if (s.length < 2) return s;
  // Linear detrend. PPG rides on a large slow baseline and IMU on gravity, and
  // leaving either in place buries the oscillation being looked for.
  const n = s.length;
  let sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (let i = 0; i < n; i++) { sx += i; sy += s[i]; sxx += i * i; sxy += i * s[i]; }
  const d = n * sxx - sx * sx;
  const slope = d !== 0 ? (n * sxy - sx * sy) / d : 0;
  const icpt = (sy - slope * sx) / n;
  return s.map((x, i) => x - (slope * i + icpt));
}

// One-pole band-pass built from two exponential smoothers. Cheap enough to run
// four times a second on a phone.
function bandpass(v, sr, lo, hi) {
  const a = Math.exp(-2 * Math.PI * lo / sr);
  const b = Math.exp(-2 * Math.PI * hi / sr);
  let slow = v[0] ?? 0, fast = v[0] ?? 0;
  const out = new Array(v.length);
  for (let i = 0; i < v.length; i++) {
    slow = a * slow + (1 - a) * v[i];
    fast = b * fast + (1 - b) * v[i];
    out[i] = fast - slow;
  }
  return out;
}

// Beats per minute from the IR channel.
//
// Peaks above a threshold with a refractory gap, then the mean interval --
// the same shape as heart_from_ppg, minus the quality machinery. Returns 0
// rather than dividing by zero when nothing beat, because 0 is a sentinel the
// display can show as "--" while NaN would propagate.
export function heartRate(v, sr) {
  const s = bandpass(detrend(v), sr, 0.5, 4.0);
  if (s.length < sr * 5) return { bpm: 0, beats: 0 };

  let mean = 0;
  for (const x of s) mean += x;
  mean /= s.length;
  let sd = 0;
  for (const x of s) sd += (x - mean) * (x - mean);
  sd = Math.sqrt(sd / s.length);
  const thr = mean + 0.5 * sd;

  const refractory = Math.round(0.3 * sr);   // 300 ms, so 200 BPM is the ceiling
  const peaks = [];
  for (let i = 1; i < s.length - 1; i++) {
    if (s[i] <= thr || s[i] < s[i - 1] || s[i] < s[i + 1]) continue;
    if (peaks.length && i - peaks[peaks.length - 1] < refractory) continue;
    peaks.push(i);
  }
  if (peaks.length < 3) return { bpm: 0, beats: peaks.length };

  // Intervals outside 0.25-2.0 s are not heartbeats; averaging them in is how
  // a movement artifact becomes a plausible-looking BPM.
  const ibis = [];
  for (let i = 1; i < peaks.length; i++) {
    const dt = (peaks[i] - peaks[i - 1]) / sr;
    if (dt >= 0.25 && dt <= 2.0) ibis.push(dt);
  }
  if (!ibis.length) return { bpm: 0, beats: peaks.length };
  const mIbi = ibis.reduce((a, b) => a + b, 0) / ibis.length;
  return { bpm: 60 / mIbi, beats: peaks.length };
}

// Breaths per minute from head motion.
//
// Breathing drives small slow head movement. The axis with the most in-band
// variance wins, and its name is reported because which axis carries the
// signal depends on how the headset happens to sit.
export function breathRate(axes, sr) {
  let best = -1, bestVar = 0;
  const names = ["ax", "ay", "az", "gx", "gy", "gz"];
  const filtered = [];

  for (let a = 0; a < axes.length; a++) {
    const f = bandpass(detrend(axes[a]), sr, 0.1, 0.5);
    filtered.push(f);
    let m = 0;
    for (const x of f) m += x;
    m /= (f.length || 1);
    let vv = 0;
    for (const x of f) vv += (x - m) * (x - m);
    vv /= (f.length || 1);
    if (vv > bestVar) { bestVar = vv; best = a; }
  }
  if (best < 0 || filtered[best].length < sr * 20) {
    return { brpm: 0, axis: "", cycles: 0 };
  }

  // Positive-going zero crossings, which is a cycle count that does not care
  // about amplitude.
  const s = filtered[best];
  const cross = [];
  for (let i = 1; i < s.length; i++) {
    if (s[i - 1] <= 0 && s[i] > 0) cross.push(i);
  }
  if (cross.length < 2) return { brpm: 0, axis: names[best], cycles: 0 };

  const span = (cross[cross.length - 1] - cross[0]) / sr;
  const brpm = span > 0 ? (cross.length - 1) / span * 60 : 0;
  return { brpm, axis: names[best], cycles: cross.length - 1 };
}

// Summed variance of the six axes band-passed 1-10 Hz.
//
// Deliberately above the respiratory band so normal breathing cannot trip it.
// This is the substitute for the EMG channel the Muse does not have: jaw
// clench and movement are inferred here and used to discount suspect gamma.
export function motionEnergy(axes, sr) {
  let total = 0;
  for (const axis of axes) {
    const f = bandpass(detrend(axis), sr, 1.0, 10.0);
    if (!f.length) continue;
    let m = 0;
    for (const x of f) m += x;
    m /= f.length;
    for (const x of f) total += (x - m) * (x - m) / f.length;
  }
  return Math.sqrt(total);
}
