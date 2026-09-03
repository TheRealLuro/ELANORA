#include "elanora/data/eeg_features.hpp"

#include <algorithm>
#include <cmath>

#include "elanora/data/dsp.hpp"

namespace elanora::data {

BandPowers band_powers(const std::vector<double>& samples, int sampling_rate) {
    BandPowers bp{};
    if (samples.size() < 32 || sampling_rate <= 0) return bp;

    const double fs = static_cast<double>(sampling_rate);

    std::vector<double> s = samples;
    fill_gaps(s);
    detrend_linear(s);

    // Band-limit before the transform. The 1 Hz corner removes electrode drift
    // that would otherwise pile into delta, and the 45 Hz corner keeps the
    // 60 Hz mains peak and its filter skirt out of gamma.
    s = bandpass(s, kAnalysisLowHz, kAnalysisHighHz, fs, 4);

    // 1024 points at 256 Hz gives 0.25 Hz bins -- fine enough that the 4 Hz and
    // 8 Hz band edges land cleanly -- and about 14 half-overlapped segments
    // over a 30 s period, which is enough averaging to stabilise the estimate.
    const Psd psd = welch_psd(s, fs, 1024);
    if (psd.empty()) return bp;

    for (int i = 0; i < kBandCount; ++i) {
        bp.abs[static_cast<std::size_t>(i)] =
            band_power(psd, kBandEdges[static_cast<std::size_t>(i)][0],
                       kBandEdges[static_cast<std::size_t>(i)][1]);
    }
    bp.total = band_power(psd, kAnalysisLowHz, kAnalysisHighHz);

    for (int i = 0; i < kBandCount; ++i) {
        bp.rel[static_cast<std::size_t>(i)] =
            (bp.total > 0.0) ? bp.abs[static_cast<std::size_t>(i)] / bp.total : 0.0;
    }
    return bp;
}

Dominance assess_dominance(const BandPowers& bp, double codominance_threshold) {
    Dominance d;

    int top = 0;
    for (int i = 1; i < kBandCount; ++i) {
        if (bp.rel[static_cast<std::size_t>(i)] > bp.rel[static_cast<std::size_t>(top)]) top = i;
    }
    d.dominant = static_cast<Band>(top);

    double second = -1.0;
    for (int i = 0; i < kBandCount; ++i) {
        if (i == top) continue;
        second = std::max(second, bp.rel[static_cast<std::size_t>(i)]);
    }
    const double lead = bp.rel[static_cast<std::size_t>(top)];
    d.margin = (second < 0.0) ? 0.0 : lead - second;

    // Every band close enough to the leader is reported with it. A margin of
    // 0.02 between alpha and theta is not a finding about alpha; forcing a
    // single label there would invent a distinction the data cannot support.
    for (int i = 0; i < kBandCount; ++i) {
        if (lead - bp.rel[static_cast<std::size_t>(i)] <= codominance_threshold) {
            d.codominant.push_back(static_cast<Band>(i));
        }
    }
    return d;
}

}  // namespace elanora::data
