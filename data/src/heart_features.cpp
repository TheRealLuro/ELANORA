#include "elanora/data/heart_features.hpp"

#include <algorithm>
#include <cmath>

#include "elanora/data/dsp.hpp"

namespace elanora::data {

namespace {

// Physiological bounds on an inter-beat interval. 0.25 s is 240 BPM and 2.0 s
// is 30 BPM; anything outside is a detection error, not a heartbeat, and
// averaging it in would corrupt both BPM and RMSSD.
constexpr double kMinIbi = 0.25;
constexpr double kMaxIbi = 2.00;

}  // namespace

HeartFeatures heart_from_ppg(const std::vector<double>& ppg_ir, int sampling_rate) {
    HeartFeatures hf{};
    if (ppg_ir.size() < 32 || sampling_rate <= 0) return hf;

    const double fs = static_cast<double>(sampling_rate);

    // 0.5-4.0 Hz spans 30-240 BPM. Zero phase, because a causal filter would
    // shift every peak by its group delay and bias every interval identically
    // -- invisible in BPM but not in RMSSD.
    std::vector<double> s = bandpass(ppg_ir, 0.5, 4.0, fs, 4);
    if (s.empty()) return hf;

    const double sd = std::sqrt(variance_of(s));
    if (sd <= 0.0) return hf;   // flat input: no beats, and no divide by zero

    // Threshold at half a standard deviation above the median. The median is
    // used rather than the mean because a motion artifact is a large one-sided
    // excursion that drags a mean far more than it drags a median.
    const double thresh = median_of(s) + 0.5 * sd;
    const int refractory = std::max(1, static_cast<int>(0.3 * fs));

    std::vector<double> peaks;   // times, seconds from the start of the window
    int last = -refractory;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] <= thresh) continue;
        if (!(s[i] > s[i - 1] && s[i] >= s[i + 1])) continue;
        if (static_cast<int>(i) - last < refractory) continue;
        peaks.push_back(static_cast<double>(i) / fs);
        last = static_cast<int>(i);
    }
    if (peaks.size() < 2) return hf;

    std::vector<double> ibi;
    int rejected = 0;
    for (std::size_t i = 1; i < peaks.size(); ++i) {
        const double d = peaks[i] - peaks[i - 1];
        if (d >= kMinIbi && d <= kMaxIbi) ibi.push_back(d);
        else ++rejected;
    }
    if (ibi.empty()) return hf;

    hf.n_beats = static_cast<int>(ibi.size()) + 1;
    hf.bpm     = 60.0 / mean_of(ibi);

    if (ibi.size() >= 2) {
        double acc = 0.0;
        for (std::size_t i = 1; i < ibi.size(); ++i) {
            const double d_ms = (ibi[i] - ibi[i - 1]) * 1000.0;
            acc += d_ms * d_ms;
        }
        hf.rmssd = std::sqrt(acc / static_cast<double>(ibi.size() - 1));
    }

    // Quality is acceptance rate times regularity. A run of intervals that are
    // all plausible but wildly inconsistent is a detector locking onto the
    // dicrotic notch as often as the systolic peak, which is a bad estimate
    // even though every interval passed the range check.
    const double accepted = static_cast<double>(ibi.size()) /
                            static_cast<double>(ibi.size() + rejected);
    const double m = mean_of(ibi);
    const double cv = (m > 0.0) ? std::sqrt(variance_of(ibi)) / m : 1.0;
    const double regularity = std::clamp(1.0 - cv / 0.30, 0.0, 1.0);
    hf.quality = std::clamp(accepted * regularity, 0.0, 1.0);

    return hf;
}

}  // namespace elanora::data
