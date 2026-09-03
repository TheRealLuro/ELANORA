#pragma once
//
// Breathing rate from head motion (Task 18).
//
// The Muse 2 has no respiration belt. Breathing shows up as small slow head
// movement on the accelerometer and gyroscope -- the head rocks slightly with
// the chest. Over a 30 s period at 12 breaths/min that is about 6 cycles, with
// roughly 2 breaths/min of spectral resolution: usable, which it would not have
// been at the 15 s window of the original paper design.
//
// Two independent estimators run on the same axis, and their agreement is the
// dominant term in the confidence score. A single estimator on a noisy signal
// returns a confident wrong number; two disagreeing estimators at least report
// that they disagree.

#include <array>
#include <string>
#include <vector>

namespace elanora::data {

inline constexpr int kImuAxisCount = 6;   // ax ay az gx gy gz

struct BreathFeatures {
    double      breaths_per_minute = 0.0;  // interval-based estimate (primary)
    double      bpm_spectral       = 0.0;  // periodogram peak (cross-check)
    int         n_cycles           = 0;
    std::string axis_used;                 // "ax".."gz", empty when nothing was usable
    double      confidence         = 0.0;  // [0,1]
};

// Axes in BrainFlow order: accel x/y/z then gyro x/y/z.
//
// `motion_energy` is the 1-10 Hz motion score from qc.hpp. It is passed in so
// that a period containing a head turn reports low confidence: gross movement
// produces a large slow component that the estimators will happily read as
// breathing. Pass 0 when it has not been computed.
BreathFeatures breath_from_imu(const std::array<std::vector<double>, kImuAxisCount>& axes,
                               int sampling_rate, double motion_energy = 0.0);

const char* imu_axis_name(int index);

}  // namespace elanora::data
