#pragma once
//
// Signal processing shared by every feature extractor.
//
// This is implemented here rather than delegated to BrainFlow's DataFilter for
// two reasons, both load-bearing:
//
//   1. Gaps. A dropped BLE packet is stored as NaN. BrainFlow's transforms
//      propagate a single NaN across the entire output; the plan requires gaps
//      be windowed to zero so one lost packet costs one bin's worth of energy
//      instead of the whole trial. That behaviour has to live in our code.
//
//   2. Testability. data/ is pure math with no device dependency, so every
//      feature can be verified on synthesised signals in CI on a machine with
//      no headset and no Bluetooth radio.
//
// Everything here is zero-phase where phase matters. Peak-picking on a
// causally filtered signal measures the filter's group delay as much as the
// physiology, and at 0.1-0.5 Hz that delay is seconds.

#include <cstddef>
#include <vector>

namespace elanora::data {

// ---------------------------------------------------------------------------
// Gap handling
// ---------------------------------------------------------------------------

// Replaces NaN with linear interpolation between the nearest finite
// neighbours, and reports what fraction was missing. Leading and trailing gaps
// are filled with the nearest finite value rather than extrapolated.
//
// Returns the fraction of samples that were not finite, in [0,1]. A caller
// that sees a high fraction should lower its confidence rather than trust the
// interpolated result.
double fill_gaps(std::vector<double>& x);

// True if every sample is finite.
bool all_finite(const std::vector<double>& x);

// ---------------------------------------------------------------------------
// Detrending
// ---------------------------------------------------------------------------

// Removes the mean.
void detrend_constant(std::vector<double>& x);

// Removes the least-squares straight line. Slow electrode drift is a ramp, and
// leaving it in dumps power into delta that has nothing to do with the brain.
void detrend_linear(std::vector<double>& x);

// ---------------------------------------------------------------------------
// Butterworth filtering
// ---------------------------------------------------------------------------

// One second-order section in direct form II transposed.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0;
    double a1 = 0, a2 = 0;   // a0 is normalised to 1
};

// Cascades of second-order sections. Only even orders are supported: an odd
// order needs a first-order section, and no filter in this project wants one.
std::vector<Biquad> butter_lowpass(int order, double cutoff_hz, double fs);
std::vector<Biquad> butter_highpass(int order, double cutoff_hz, double fs);
std::vector<Biquad> butter_bandpass(int order, double lo_hz, double hi_hz, double fs);

// Single forward pass. Introduces phase distortion; use only where phase does
// not matter.
std::vector<double> sosfilt(const std::vector<Biquad>& sos, const std::vector<double>& x);

// Forward-backward pass: zero phase, and the squared magnitude response of the
// single pass. Reflection padding suppresses the start-up transient, which for
// a 0.1 Hz filter would otherwise swallow the first several seconds.
std::vector<double> filtfilt(const std::vector<Biquad>& sos, const std::vector<double>& x);

// Convenience: detrend then zero-phase band-pass.
std::vector<double> bandpass(const std::vector<double>& x, double lo_hz, double hi_hz,
                             double fs, int order = 4);

// ---------------------------------------------------------------------------
// Spectra
// ---------------------------------------------------------------------------

// One-sided power spectral density, units of (signal units)^2 per Hz.
struct Psd {
    std::vector<double> freq;   // bin centres, 0 .. fs/2
    std::vector<double> power;  // same length as freq
    double df = 0.0;            // bin width

    bool empty() const { return freq.empty(); }
};

// Welch's method: Hann window, 50% overlap, linear detrend per segment.
//
// Normalised so that integrating the whole PSD recovers the signal variance --
// a sine of amplitude A integrates to A^2/2. Without that guarantee, absolute
// band power would be in arbitrary units and no two runs would be comparable.
//
// `nfft` is clamped to the signal length. If the signal is shorter than one
// segment the whole signal becomes a single segment.
Psd welch_psd(const std::vector<double>& x, double fs, int nfft = 1024);

// Single Hann-windowed periodogram over the whole record. Used where frequency
// resolution matters more than variance -- breathing at 0.2 Hz needs every bin
// the 30 s window can give, and Welch would trade them away for smoothing.
Psd periodogram(const std::vector<double>& x, double fs);

// Integrates the PSD over [lo, hi). Partial bins at the edges are included in
// proportion to their overlap, so a band edge that falls mid-bin does not
// quantise the result.
double band_power(const Psd& psd, double lo_hz, double hi_hz);

// Frequency of the largest bin strictly inside [lo, hi), refined by parabolic
// interpolation against its neighbours. Returns 0 when the range holds no bins.
double peak_frequency(const Psd& psd, double lo_hz, double hi_hz);

// ---------------------------------------------------------------------------
// Small statistics used by several extractors
// ---------------------------------------------------------------------------

double mean_of(const std::vector<double>& x);
double variance_of(const std::vector<double>& x);
double rms_of(const std::vector<double>& x);
double median_of(std::vector<double> x);   // by value: it sorts

// Radix-2 in-place complex FFT. Exposed for testing.
void fft_radix2(std::vector<double>& re, std::vector<double>& im, bool inverse = false);

}  // namespace elanora::data
