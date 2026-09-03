#include "elanora/data/qc.hpp"

#include <algorithm>
#include <cmath>

#include "elanora/data/dsp.hpp"
#include "elanora/data/heart_features.hpp"

namespace elanora::data {

namespace {

// A channel past this for more than a fraction of the period is not measuring
// EEG; the Muse's amplifier is well into saturation.
constexpr double kRailUv        = 250.0;
constexpr double kRailFraction  = 0.01;

// Resting scalp EEG sits in the single-digit to low-tens microvolt range.
// Below 1 uV RMS the electrode has lost contact; above 100 uV it is dominated
// by artifact whatever the source.
constexpr double kGoodLoUv = 3.0;
constexpr double kGoodHiUv = 50.0;
constexpr double kFairHiUv = 100.0;

// Normalising constant for motion energy. Chosen so that ordinary small head
// adjustments land well under 1 and a deliberate turn saturates it.
constexpr double kMotionFullScale = 0.02;
constexpr double kMotionThreshold = 0.25;

}  // namespace

double score_eeg_channel(const std::vector<double>& samples, int sampling_rate,
                         bool* excursion) {
    if (excursion) *excursion = false;
    if (samples.size() < 16 || sampling_rate <= 0) return 0.0;

    std::vector<double> s = samples;

    // A gap is missing data, not silence. Counting NaN as zero would drag RMS
    // down and mislabel a good electrode as dead -- the exact inversion of what
    // this function is for.
    const double missing = fill_gaps(s);
    if (missing > 0.5) return 0.0;

    // Railed samples are counted on the raw signal, before detrending, because
    // saturation is an absolute condition of the amplifier.
    std::size_t railed = 0;
    for (double v : s) {
        if (std::abs(v) > kRailUv) ++railed;
    }
    const double rail_frac = static_cast<double>(railed) / static_cast<double>(s.size());
    if (rail_frac > kRailFraction) {
        if (excursion) *excursion = true;
        return 0.0;
    }
    if (railed > 0 && excursion) *excursion = true;

    detrend_linear(s);
    const double var = variance_of(s);
    if (var < 1e-6) return 0.0;             // flat: electrode not connected

    const double rms = rms_of(s);

    // Piecewise: full marks inside the physiological band, tapering out on both
    // sides rather than a cliff, so a borderline electrode is visibly borderline
    // instead of flipping between 1.0 and 0.0 between periods.
    double score;
    if (rms >= kGoodLoUv && rms <= kGoodHiUv) {
        score = 1.0;
    } else if (rms < kGoodLoUv) {
        score = std::clamp(rms / kGoodLoUv, 0.0, 1.0);
    } else if (rms <= kFairHiUv) {
        score = std::clamp(1.0 - (rms - kGoodHiUv) / (kFairHiUv - kGoodHiUv), 0.0, 1.0) * 0.6 + 0.2;
    } else {
        score = std::clamp(0.2 * kFairHiUv / rms, 0.0, 0.2);
    }

    // Missing data scales the result down proportionally: half a period of
    // dropouts is at best half a measurement.
    return std::clamp(score * (1.0 - missing), 0.0, 1.0);
}

SignalQc assess_period(const std::vector<std::vector<double>>& eeg,
                       const std::vector<double>& ppg_ir,
                       const std::array<std::vector<double>, kImuAxisCount>& imu,
                       int sr_eeg, int sr_ppg, int sr_imu) {
    SignalQc qc{};

    for (int i = 0; i < kSensorCount; ++i) {
        if (static_cast<std::size_t>(i) >= eeg.size()) continue;
        bool exc = false;
        qc.eeg[static_cast<std::size_t>(i)] =
            score_eeg_channel(eeg[static_cast<std::size_t>(i)], sr_eeg, &exc);
        if (exc) qc.eeg_excursion = true;
    }

    // The heart score is the detector's own confidence: it already accounts for
    // implausible intervals and irregularity, and duplicating that logic here
    // would let the two definitions drift apart.
    qc.heart = heart_from_ppg(ppg_ir, sr_ppg).quality;

    // Motion energy above the respiratory band. 1-10 Hz catches head turns,
    // shifting in the chair, and jaw clench transmitted through the skull,
    // while 0.1-0.5 Hz breathing passes underneath it untouched.
    if (sr_imu > 0) {
        double acc = 0.0;
        for (const std::vector<double>& axis : imu) {
            if (axis.size() < 32) continue;
            const std::vector<double> band =
                bandpass(axis, 1.0, std::min(10.0, 0.45 * sr_imu), static_cast<double>(sr_imu), 4);
            acc += variance_of(band);
        }
        qc.motion_energy = std::clamp(acc / kMotionFullScale, 0.0, 1.0);
        qc.imu_motion = qc.motion_energy > kMotionThreshold;
    }

    qc.breath = breath_from_imu(imu, sr_imu, qc.motion_energy).confidence;
    return qc;
}

}  // namespace elanora::data
