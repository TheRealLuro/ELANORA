#pragma once
//
// Per-channel EEG signal quality.
//
// Electrode contact on a dry-sensor headset like the Muse 2 is the dominant
// source of bad data, and it degrades silently: a lifted electrode still
// produces a signal, just one made of drift and mains hum rather than brain
// activity. The collector uses this to refuse to start a round on a bad
// electrode, and the feature pipeline uses it to exclude a period per-signal
// rather than discarding a whole trial.

#include <vector>

namespace elanora::device {

enum class Quality { Good, Fair, Bad };

struct ChannelQuality {
    Quality q       = Quality::Bad;
    double  rms_uv  = 0.0;   // RMS amplitude after detrending, microvolts
    bool    railed  = false; // amplifier saturated or huge motion artifact
    bool    flat    = false; // no signal at all -- disconnected or shorted
};

// Thresholds in microvolts. Resting scalp EEG sits in roughly 5-50 uV; below a
// few uV the electrode is not picking anything up, and above ~100 uV the trace
// is dominated by artifact rather than brain activity.
inline constexpr double kFlatVarianceUv2 = 1e-6;
inline constexpr double kRailedUv        = 250.0;
inline constexpr double kRailedFraction  = 0.05;
inline constexpr double kGoodMinUv       = 3.0;
inline constexpr double kGoodMaxUv       = 50.0;
inline constexpr double kFairMaxUv       = 100.0;

// `eeg_window` is one channel's samples in microvolts, as BrainFlow reports
// them. A window shorter than a few samples returns Bad rather than guessing.
ChannelQuality assess(const std::vector<double>& eeg_window, int sampling_rate);

const char* quality_name(Quality q);

}  // namespace elanora::device
