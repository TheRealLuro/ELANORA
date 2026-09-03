#pragma once
//
// Heart rate and variability from the Muse 2 PPG (Task 17).
//
// RMSSD rather than SDNN. SDNN conventionally needs 5 minutes -- 60 s at the
// absolute shortest -- and a value computed over 30 s is not comparable to any
// published reference, which makes it worse than useless: it looks like a
// standard metric while being one. RMSSD is the accepted ultra-short-window
// measure because it reflects beat-to-beat differences rather than the total
// spread, so it stabilises far faster.
//
// dBPM stays the primary heart target; RMSSD is exploratory.

#include <vector>

namespace elanora::data {

struct HeartFeatures {
    double bpm     = 0.0;   // 0 is the sentinel for "no beats found", never a guess
    double rmssd   = 0.0;   // milliseconds
    int    n_beats = 0;
    double quality = 0.0;   // [0,1]: fraction of plausible intervals x regularity
};

// `ppg_ir` is the 940 nm infrared channel (BrainFlow PPG index 1), which
// carries the strongest pulse of the three.
HeartFeatures heart_from_ppg(const std::vector<double>& ppg_ir, int sampling_rate);

}  // namespace elanora::data
