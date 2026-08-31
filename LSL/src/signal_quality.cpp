#include "elanora/lsl/signal_quality.hpp"

#include <cmath>
#include <numeric>

namespace elanora::lsl {

namespace {

// Removes the linear trend in place. Electrode drift is a slow ramp that would
// otherwise dominate the RMS and make a perfectly good channel look railed.
void detrend_linear(std::vector<double>& v) {
    const std::size_t n = v.size();
    if (n < 2) return;

    const double n_d = static_cast<double>(n);
    double sum_x = 0.0, sum_y = 0.0, sum_xx = 0.0, sum_xy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        sum_x  += x;
        sum_y  += v[i];
        sum_xx += x * x;
        sum_xy += x * v[i];
    }
    const double denom = n_d * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) return;

    const double slope     = (n_d * sum_xy - sum_x * sum_y) / denom;
    const double intercept = (sum_y - slope * sum_x) / n_d;
    for (std::size_t i = 0; i < n; ++i) {
        v[i] -= slope * static_cast<double>(i) + intercept;
    }
}

}  // namespace

ChannelQuality assess(const std::vector<double>& eeg_window, int sampling_rate) {
    (void)sampling_rate;  // reserved: line-noise ratio is a later refinement

    ChannelQuality out;
    if (eeg_window.size() < 8) return out;  // Bad: too short to judge

    // Railing is measured on the raw signal. Detrending first would hide a
    // saturated amplifier by folding the excursion into the trend.
    std::size_t n_extreme = 0;
    bool all_finite = true;
    for (double s : eeg_window) {
        if (!std::isfinite(s)) { all_finite = false; break; }
        if (std::abs(s) > kRailedUv) ++n_extreme;
    }
    if (!all_finite) return out;  // Bad

    out.railed = static_cast<double>(n_extreme) >
                 kRailedFraction * static_cast<double>(eeg_window.size());

    std::vector<double> work = eeg_window;
    detrend_linear(work);

    double sum_sq = 0.0;
    for (double s : work) sum_sq += s * s;
    const double variance = sum_sq / static_cast<double>(work.size());
    out.rms_uv = std::sqrt(variance);

    out.flat = variance < kFlatVarianceUv2;

    if (out.flat || out.railed) {
        out.q = Quality::Bad;
    } else if (out.rms_uv >= kGoodMinUv && out.rms_uv <= kGoodMaxUv) {
        out.q = Quality::Good;
    } else if (out.rms_uv > kGoodMaxUv && out.rms_uv <= kFairMaxUv) {
        out.q = Quality::Fair;
    } else {
        // Below kGoodMinUv but not flat: the electrode is attached but barely
        // picking anything up, which is as unusable as no contact at all.
        out.q = Quality::Bad;
    }
    return out;
}

const char* quality_name(Quality q) {
    switch (q) {
        case Quality::Good: return "good";
        case Quality::Fair: return "fair";
        case Quality::Bad:  return "bad";
    }
    return "unknown";
}

}  // namespace elanora::lsl
