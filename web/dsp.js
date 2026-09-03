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
