#include "elanora/data/breath_features.hpp"

#include <algorithm>
#include <cmath>

#include "elanora/data/dsp.hpp"

namespace elanora::data {

namespace {

// 0.1-0.5 Hz is 6-30 breaths/min, which brackets every resting rate and
// excludes both postural drift below and any deliberate movement above.
constexpr double kBreathLoHz = 0.10;
constexpr double kBreathHiHz = 0.50;

const char* kAxisNames[kImuAxisCount] = {"ax", "ay", "az", "gx", "gy", "gz"};

// Positive-going zero crossings of a zero-mean band-passed signal mark one
// point per cycle. Counting peaks instead would double-count a waveform with a
// shoulder, which chest motion often has.
int count_up_crossings(const std::vector<double>& x, std::vector<double>& times, double fs) {
    times.clear();
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0 && x[i] >= 0.0) {
            // Linear interpolation to sub-sample precision. At 52 Hz one sample
            // is 0.4% of a 5 s breath, but the error accumulates across a short
            // record and biases the mean interval.
            const double frac = (x[i] != x[i - 1]) ? -x[i - 1] / (x[i] - x[i - 1]) : 0.0;
            times.push_back((static_cast<double>(i - 1) + frac) / fs);
        }
    }
    return static_cast<int>(times.size());
}

}  // namespace

const char* imu_axis_name(int index) {
    if (index < 0 || index >= kImuAxisCount) return "";
    return kAxisNames[index];
}

BreathFeatures breath_from_imu(const std::array<std::vector<double>, kImuAxisCount>& axes,
                               int sampling_rate, double motion_energy) {
    BreathFeatures bf{};
    if (sampling_rate <= 0) return bf;
    const double fs = static_cast<double>(sampling_rate);

    // Pick the axis carrying the most in-band energy. Which axis breathing
    // lands on depends on how the headset sits, so it cannot be fixed in
    // advance -- and recording which one was used makes that visible later.
    int best_axis = -1;
    double best_var = 0.0;
    double best_ratio = 0.0;
    std::vector<double> best_filtered;

    for (int a = 0; a < kImuAxisCount; ++a) {
        if (axes[static_cast<std::size_t>(a)].size() < 32) continue;

        std::vector<double> raw = axes[static_cast<std::size_t>(a)];
        fill_gaps(raw);
        detrend_linear(raw);

        const std::vector<double> band = bandpass(raw, kBreathLoHz, kBreathHiHz, fs, 4);
        const double in_band  = variance_of(band);
        const double total    = variance_of(raw);
        const double ratio    = (total > 0.0) ? in_band / total : 0.0;

        if (in_band > best_var) {
            best_var      = in_band;
            best_axis     = a;
            best_ratio    = ratio;
            best_filtered = band;
        }
    }

    if (best_axis < 0 || best_filtered.empty()) return bf;
    bf.axis_used = kAxisNames[best_axis];

    // Primary estimate: mean interval between successive up-crossings.
    std::vector<double> crossings;
    const int n_cross = count_up_crossings(best_filtered, crossings, fs);
    if (n_cross >= 2) {
        const double span = crossings.back() - crossings.front();
        const double cycles = static_cast<double>(n_cross - 1);
        if (span > 0.0) bf.breaths_per_minute = 60.0 * cycles / span;
        bf.n_cycles = static_cast<int>(cycles);
    }

    // Cross-check: dominant peak of a single periodogram over the whole record.
    // Not Welch -- splitting 30 s into overlapping segments would halve the
    // frequency resolution, and resolution is exactly what makes a 30 s window
    // worth having at this rate.
    const Psd psd = periodogram(best_filtered, fs);
    const double peak_hz = peak_frequency(psd, kBreathLoHz, kBreathHiHz);
    bf.bpm_spectral = peak_hz * 60.0;

    // Confidence is a product of four terms, each in [0,1]. A product rather
    // than an average: any one of these failing should sink the estimate, and
    // an average would let three good terms carry one disqualifying one.
    const double c_cycles = std::clamp(static_cast<double>(bf.n_cycles) / 6.0, 0.0, 1.0);

    double c_agree = 0.0;
    if (bf.breaths_per_minute > 0.0 && bf.bpm_spectral > 0.0) {
        const double hi = std::max(bf.breaths_per_minute, bf.bpm_spectral);
        c_agree = std::clamp(1.0 - std::abs(bf.breaths_per_minute - bf.bpm_spectral) / hi,
                             0.0, 1.0);
    }

    // In-band variance ratio. 0.5 of the total residing in the respiratory band
    // is already a strong signal for a sensor that is not a respiration belt.
    const double c_band = std::clamp(best_ratio / 0.5, 0.0, 1.0);

    // Gross movement produces a large slow component that both estimators will
    // read as breathing, so it has to discount the result directly.
    const double c_motion = std::clamp(1.0 - motion_energy, 0.0, 1.0);

    bf.confidence = std::clamp(c_cycles * c_agree * c_band * c_motion, 0.0, 1.0);
    return bf;
}

}  // namespace elanora::data
