#include "elanora/data/dsp.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace elanora::data {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool finite(double v) { return std::isfinite(v); }

int next_pow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gaps
// ---------------------------------------------------------------------------

bool all_finite(const std::vector<double>& x) {
    for (double v : x) {
        if (!finite(v)) return false;
    }
    return true;
}

double fill_gaps(std::vector<double>& x) {
    const std::size_t n = x.size();
    if (n == 0) return 0.0;

    std::size_t missing = 0;
    for (double v : x) {
        if (!finite(v)) ++missing;
    }
    if (missing == 0) return 0.0;
    if (missing == n) {
        // Nothing to interpolate from. Zero is the honest answer: it carries no
        // energy into any band, and the caller sees a missing fraction of 1.
        std::fill(x.begin(), x.end(), 0.0);
        return 1.0;
    }

    // Leading gap: hold the first finite value backwards.
    std::size_t first = 0;
    while (first < n && !finite(x[first])) ++first;
    for (std::size_t i = 0; i < first; ++i) x[i] = x[first];

    // Trailing gap: hold the last finite value forwards.
    std::size_t last = n - 1;
    while (last > 0 && !finite(x[last])) --last;
    for (std::size_t i = last + 1; i < n; ++i) x[i] = x[last];

    // Interior gaps: straight line between the finite samples that bracket them.
    std::size_t i = first;
    while (i <= last) {
        if (finite(x[i])) { ++i; continue; }
        const std::size_t gap_start = i;
        std::size_t gap_end = i;
        while (gap_end <= last && !finite(x[gap_end])) ++gap_end;

        const double a = x[gap_start - 1];
        const double b = x[gap_end];
        const double span = static_cast<double>(gap_end - gap_start + 1);
        for (std::size_t k = gap_start; k < gap_end; ++k) {
            const double t = static_cast<double>(k - gap_start + 1) / span;
            x[k] = a + (b - a) * t;
        }
        i = gap_end;
    }
    return static_cast<double>(missing) / static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// Detrending
// ---------------------------------------------------------------------------

void detrend_constant(std::vector<double>& x) {
    if (x.empty()) return;
    const double m = mean_of(x);
    for (double& v : x) v -= m;
}

void detrend_linear(std::vector<double>& x) {
    const std::size_t n = x.size();
    if (n < 2) { detrend_constant(x); return; }

    // Regress on the centred index so the normal equations stay well
    // conditioned even for a long record.
    const double mid = 0.5 * static_cast<double>(n - 1);
    double sxy = 0.0, sxx = 0.0, sy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) - mid;
        sxy += t * x[i];
        sxx += t * t;
        sy  += x[i];
    }
    const double slope = (sxx > 0.0) ? sxy / sxx : 0.0;
    const double inter = sy / static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] -= inter + slope * (static_cast<double>(i) - mid);
    }
}

// ---------------------------------------------------------------------------
// Butterworth design
// ---------------------------------------------------------------------------
//
// The analog prototype poles of an order-N Butterworth sit on the unit circle
// at angles pi*(2k+1+N)/(2N). Taken in conjugate pairs each gives a normalised
// section s^2 + a1*s + 1 with a1 = -2*Re(p). The bilinear transform with a
// prewarped cutoff maps each section to a biquad.

namespace {

// a1 coefficients of the normalised second-order sections, low to high Q.
std::vector<double> butter_sections(int order) {
    std::vector<double> a1;
    if (order < 2 || order % 2 != 0) return a1;   // even orders only
    for (int k = 0; k < order / 2; ++k) {
        const double theta = kPi * (2.0 * k + 1.0 + order) / (2.0 * order);
        a1.push_back(-2.0 * std::cos(theta));
    }
    return a1;
}

}  // namespace

std::vector<Biquad> butter_lowpass(int order, double cutoff_hz, double fs) {
    std::vector<Biquad> sos;
    if (fs <= 0.0 || cutoff_hz <= 0.0 || cutoff_hz >= 0.5 * fs) return sos;

    // c = cot(pi*fc/fs) folds the prewarping into one constant.
    const double c = 1.0 / std::tan(kPi * cutoff_hz / fs);
    const double c2 = c * c;

    for (double a1 : butter_sections(order)) {
        const double d0 = c2 + a1 * c + 1.0;
        Biquad q;
        q.b0 = 1.0 / d0;
        q.b1 = 2.0 / d0;
        q.b2 = 1.0 / d0;
        q.a1 = (2.0 - 2.0 * c2) / d0;
        q.a2 = (c2 - a1 * c + 1.0) / d0;
        sos.push_back(q);
    }
    return sos;
}

std::vector<Biquad> butter_highpass(int order, double cutoff_hz, double fs) {
    std::vector<Biquad> sos;
    if (fs <= 0.0 || cutoff_hz <= 0.0 || cutoff_hz >= 0.5 * fs) return sos;

    const double c = std::tan(kPi * cutoff_hz / fs);
    const double c2 = c * c;

    for (double a1 : butter_sections(order)) {
        const double d0 = c2 + a1 * c + 1.0;
        Biquad q;
        q.b0 = 1.0 / d0;
        q.b1 = -2.0 / d0;
        q.b2 = 1.0 / d0;
        q.a1 = (2.0 * c2 - 2.0) / d0;
        q.a2 = (c2 - a1 * c + 1.0) / d0;
        sos.push_back(q);
    }
    return sos;
}

std::vector<Biquad> butter_bandpass(int order, double lo_hz, double hi_hz, double fs) {
    // A high-pass and a low-pass in series rather than a true bandpass
    // transformation. For the wide bands here the difference in skirt shape is
    // immaterial, and this form cannot produce the near-unstable sections a
    // narrow direct bandpass design does at 0.1 Hz on a 52 Hz stream.
    std::vector<Biquad> sos = butter_highpass(order, lo_hz, fs);
    const std::vector<Biquad> lp = butter_lowpass(order, hi_hz, fs);
    sos.insert(sos.end(), lp.begin(), lp.end());
    return sos;
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

std::vector<double> sosfilt(const std::vector<Biquad>& sos, const std::vector<double>& x) {
    std::vector<double> y = x;
    for (const Biquad& q : sos) {
        double z1 = 0.0, z2 = 0.0;
        for (double& v : y) {
            const double in = v;
            const double out = q.b0 * in + z1;
            z1 = q.b1 * in - q.a1 * out + z2;
            z2 = q.b2 * in - q.a2 * out;
            v = out;
        }
    }
    return y;
}

// How many samples the slowest section needs to decay to `eps`. A 1 Hz
// high-pass at 256 Hz has poles at radius 0.991 and needs roughly 700 samples;
// a fixed pad chosen without reference to the poles is either wasteful or, far
// worse, silently too short.
static std::size_t settling_length(const std::vector<Biquad>& sos, double eps) {
    std::size_t worst = 32;
    for (const Biquad& q : sos) {
        const double disc = q.a1 * q.a1 - 4.0 * q.a2;
        double radius;
        if (disc < 0.0) {
            radius = std::sqrt(std::max(q.a2, 0.0));         // complex pair
        } else {
            const double r = std::sqrt(disc);
            radius = std::max(std::abs((-q.a1 + r) * 0.5), std::abs((-q.a1 - r) * 0.5));
        }
        if (radius <= 0.0 || radius >= 1.0) continue;
        const auto need = static_cast<std::size_t>(std::log(eps) / std::log(radius)) + 1;
        worst = std::max(worst, need);
    }
    return worst;
}

std::vector<double> filtfilt(const std::vector<Biquad>& sos, const std::vector<double>& x) {
    const std::size_t n = x.size();
    if (sos.empty() || n < 4) return x;

    // Mirror reflection, NOT point reflection.
    //
    // The textbook choice is odd (point) reflection, 2*x[0] - x[k], which is
    // continuous in both value and slope. It is also wrong here: it offsets the
    // padded region by 2*x[endpoint], and that DC step makes a high-pass ring
    // for hundreds of samples. The forward pass parks that ringing just outside
    // the data, and then the reverse pass drags it straight back in. Measured
    // on a 60 Hz tone through a 1-45 Hz band-pass, odd reflection leaks 5x the
    // stopband amplitude that mirror reflection does, and -- the part that makes
    // it a trap -- a longer pad makes it slightly worse rather than better.
    //
    // Mirror reflection has a slope discontinuity but introduces no DC step, so
    // nothing rings. That trade is strongly favourable for every filter here.
    const std::size_t pad = std::min<std::size_t>(n - 1, settling_length(sos, 1e-3));

    std::vector<double> ext;
    ext.reserve(n + 2 * pad);
    for (std::size_t i = 0; i < pad; ++i) ext.push_back(x[pad - i]);
    ext.insert(ext.end(), x.begin(), x.end());
    for (std::size_t i = 0; i < pad; ++i) ext.push_back(x[n - 2 - i]);

    std::vector<double> f = sosfilt(sos, ext);
    std::reverse(f.begin(), f.end());
    f = sosfilt(sos, f);
    std::reverse(f.begin(), f.end());

    return std::vector<double>(f.begin() + static_cast<std::ptrdiff_t>(pad),
                               f.begin() + static_cast<std::ptrdiff_t>(pad + n));
}

std::vector<double> bandpass(const std::vector<double>& x, double lo_hz, double hi_hz,
                             double fs, int order) {
    std::vector<double> y = x;
    fill_gaps(y);
    detrend_linear(y);
    const std::vector<Biquad> sos = butter_bandpass(order, lo_hz, hi_hz, fs);
    if (sos.empty()) return y;
    return filtfilt(sos, y);
}

// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------

void fft_radix2(std::vector<double>& re, std::vector<double>& im, bool inverse) {
    const std::size_t n = re.size();
    if (n < 2 || (n & (n - 1)) != 0 || im.size() != n) return;

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }

    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = sign * 2.0 * kPi / static_cast<double>(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (std::size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::size_t a = i + k, b = i + k + len / 2;
                const double xr = re[b] * cr - im[b] * ci;
                const double xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr;  im[b] = im[a] - xi;
                re[a] += xr;         im[a] += xi;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
    if (inverse) {
        for (std::size_t i = 0; i < n; ++i) { re[i] /= static_cast<double>(n); im[i] /= static_cast<double>(n); }
    }
}

// ---------------------------------------------------------------------------
// Spectra
// ---------------------------------------------------------------------------

namespace {

// One Hann-windowed segment, accumulated into `acc`. Returns the window's power
// normalisation so the caller can scale once at the end.
double accumulate_segment(const std::vector<double>& seg, std::size_t nfft,
                          std::vector<double>& acc) {
    std::vector<double> re(nfft, 0.0), im(nfft, 0.0);

    double u = 0.0;
    for (std::size_t i = 0; i < seg.size(); ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                              static_cast<double>(seg.size() - 1));
        re[i] = seg[i] * w;
        u += w * w;
    }
    u /= static_cast<double>(seg.size());

    fft_radix2(re, im, false);
    for (std::size_t k = 0; k < acc.size(); ++k) {
        acc[k] += re[k] * re[k] + im[k] * im[k];
    }
    return u;
}

Psd finish_psd(std::vector<double> acc, std::size_t nfft, double fs, int n_segments,
               double u, std::size_t seg_len) {
    Psd psd;
    if (n_segments <= 0 || u <= 0.0) return psd;

    psd.df = fs / static_cast<double>(nfft);
    psd.freq.resize(acc.size());
    psd.power.resize(acc.size());

    // Scale so that sum(power)*df == variance of the input. The 2x on the
    // interior bins folds the negative frequencies onto the positive axis; DC
    // and Nyquist have no mirror and keep their single count.
    const double norm = 1.0 / (fs * static_cast<double>(seg_len) * u *
                               static_cast<double>(n_segments));
    for (std::size_t k = 0; k < acc.size(); ++k) {
        const bool edge = (k == 0) || (k + 1 == acc.size() && nfft % 2 == 0);
        psd.freq[k]  = static_cast<double>(k) * psd.df;
        psd.power[k] = acc[k] * norm * (edge ? 1.0 : 2.0);
    }
    return psd;
}

}  // namespace

Psd welch_psd(const std::vector<double>& x, double fs, int nfft) {
    Psd empty;
    if (x.empty() || fs <= 0.0) return empty;

    std::vector<double> sig = x;
    fill_gaps(sig);

    std::size_t seg = static_cast<std::size_t>(nfft > 0 ? nfft : 1024);
    if (seg > sig.size()) seg = sig.size();
    if (seg < 8) return empty;

    // The transform length is the next power of two at or above the segment, so
    // a signal that is not a convenient length is zero-padded rather than
    // truncated.
    const std::size_t n = static_cast<std::size_t>(next_pow2(static_cast<int>(seg)));
    const std::size_t hop = std::max<std::size_t>(1, seg / 2);

    std::vector<double> acc(n / 2 + 1, 0.0);
    int count = 0;
    double u = 0.0;

    for (std::size_t start = 0; start + seg <= sig.size(); start += hop) {
        std::vector<double> s(sig.begin() + static_cast<std::ptrdiff_t>(start),
                              sig.begin() + static_cast<std::ptrdiff_t>(start + seg));
        detrend_linear(s);
        u = accumulate_segment(s, n, acc);
        ++count;
    }
    return finish_psd(std::move(acc), n, fs, count, u, seg);
}

Psd periodogram(const std::vector<double>& x, double fs) {
    Psd empty;
    if (x.size() < 8 || fs <= 0.0) return empty;

    std::vector<double> sig = x;
    fill_gaps(sig);
    detrend_linear(sig);

    const std::size_t seg = sig.size();
    const std::size_t n = static_cast<std::size_t>(next_pow2(static_cast<int>(seg)));
    std::vector<double> acc(n / 2 + 1, 0.0);
    const double u = accumulate_segment(sig, n, acc);
    return finish_psd(std::move(acc), n, fs, 1, u, seg);
}

double band_power(const Psd& psd, double lo_hz, double hi_hz) {
    if (psd.empty() || psd.df <= 0.0 || hi_hz <= lo_hz) return 0.0;

    double total = 0.0;
    for (std::size_t k = 0; k < psd.freq.size(); ++k) {
        // Each bin covers [f - df/2, f + df/2). Counting only whole bins would
        // make a band edge land differently depending on nfft, so partial bins
        // contribute in proportion to their overlap with the band.
        const double bin_lo = psd.freq[k] - 0.5 * psd.df;
        const double bin_hi = psd.freq[k] + 0.5 * psd.df;
        const double lo = std::max(bin_lo, lo_hz);
        const double hi = std::min(bin_hi, hi_hz);
        if (hi > lo) total += psd.power[k] * (hi - lo);
    }
    return total;
}

double peak_frequency(const Psd& psd, double lo_hz, double hi_hz) {
    if (psd.empty() || psd.df <= 0.0) return 0.0;

    std::size_t best = psd.freq.size();
    double best_p = -1.0;
    for (std::size_t k = 0; k < psd.freq.size(); ++k) {
        if (psd.freq[k] < lo_hz || psd.freq[k] >= hi_hz) continue;
        if (psd.power[k] > best_p) { best_p = psd.power[k]; best = k; }
    }
    if (best >= psd.freq.size()) return 0.0;

    // Parabolic interpolation against the neighbouring bins. Without it the
    // estimate quantises to df, which at 0.033 Hz resolution is 2 breaths/min.
    if (best > 0 && best + 1 < psd.power.size()) {
        const double a = psd.power[best - 1], b = psd.power[best], c = psd.power[best + 1];
        const double denom = a - 2.0 * b + c;
        if (std::abs(denom) > 1e-300) {
            const double shift = 0.5 * (a - c) / denom;
            if (std::abs(shift) <= 1.0) return psd.freq[best] + shift * psd.df;
        }
    }
    return psd.freq[best];
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

double mean_of(const std::vector<double>& x) {
    if (x.empty()) return 0.0;
    return std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
}

double variance_of(const std::vector<double>& x) {
    if (x.size() < 2) return 0.0;
    const double m = mean_of(x);
    double s = 0.0;
    for (double v : x) s += (v - m) * (v - m);
    return s / static_cast<double>(x.size() - 1);
}

double rms_of(const std::vector<double>& x) {
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (double v : x) s += v * v;
    return std::sqrt(s / static_cast<double>(x.size()));
}

double median_of(std::vector<double> x) {
    if (x.empty()) return 0.0;
    const std::size_t mid = x.size() / 2;
    std::nth_element(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(mid), x.end());
    const double hi = x[mid];
    if (x.size() % 2 == 1) return hi;
    std::nth_element(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(mid - 1), x.end());
    return 0.5 * (hi + x[mid - 1]);
}

}  // namespace elanora::data
