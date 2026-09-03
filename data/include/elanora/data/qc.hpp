#pragma once
//
// Per-signal quality control (Task 19).
//
// Quality is scored per signal, never per trial. A period with clean EEG and
// unusable breathing still feeds the four brain models; discarding the whole
// trial would throw away good data because a different sensor failed. This is
// the single most consequential decision in the analysis layer for how much
// usable data survives a real session.
//
// The Muse 2 has no EMG channel, so jaw clench and gross movement are inferred
// from EEG excursions and IMU motion energy. That inference is what discounts a
// gamma finding that is really masseter tension.

#include <array>
#include <vector>

#include "elanora/data/breath_features.hpp"
#include "elanora/types.hpp"

namespace elanora::data {

struct SignalQc {
    std::array<double, kSensorCount> eeg{};   // per electrode, [0,1]
    double heart = 0.0;                       // [0,1]
    double breath = 0.0;                      // [0,1]

    // Summed variance of the six IMU axes band-passed 1-10 Hz, normalised to
    // [0,1]. Deliberately above the respiratory band so ordinary breathing
    // cannot trip it.
    double motion_energy = 0.0;

    bool eeg_excursion = false;   // any channel beyond +/-250 uV for >1% of samples
    bool imu_motion    = false;   // motion_energy past the movement threshold
};

// Every argument is one period of one round. `eeg` is four channels in S1..S4
// order; pass fewer and the missing electrodes score zero.
SignalQc assess_period(const std::vector<std::vector<double>>& eeg,
                       const std::vector<double>& ppg_ir,
                       const std::array<std::vector<double>, kImuAxisCount>& imu,
                       int sr_eeg, int sr_ppg, int sr_imu);

// Exposed because the collector's live view scores a rolling window with the
// same rule the dataset will later apply. A different rule in each place would
// let an operator accept a period the analysis then discards.
double score_eeg_channel(const std::vector<double>& samples, int sampling_rate,
                         bool* excursion = nullptr);

}  // namespace elanora::data
