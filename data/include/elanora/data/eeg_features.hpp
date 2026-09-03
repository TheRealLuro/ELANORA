#pragma once
//
// EEG band power (Task 15) and band dominance (Task 16).
//
// Absolute AND relative power are always computed together. Neither is
// sufficient alone: relative power is blind to a change in overall amplitude,
// and absolute power is contaminated by electrode impedance, which varies
// between sessions and even between periods within a session. Storing both is
// what lets the analysis distinguish "alpha grew" from "everything shrank
// except alpha".

#include <vector>

#include "elanora/types.hpp"

namespace elanora::data {

// Band powers of one electrode over one period.
//
// `abs` is in signal-units squared (microvolts squared for Muse EEG); `rel` is
// each band divided by the 1-45 Hz total and sums to 1.
BandPowers band_powers(const std::vector<double>& samples, int sampling_rate);

// Which band leads, and by how much.
//
// The paper design asked for a "dominant band" without defining it. This is the
// definition: the argmax of relative power, its margin over the runner-up, and
// every band close enough to the top that calling a single winner would
// overstate the data.
struct Dominance {
    Band   dominant = Band::Delta;
    double margin   = 0.0;          // top relative power minus second
    std::vector<Band> codominant;   // every band within the threshold of the top
};

// `codominance_threshold` is in units of relative power: a band within this
// distance of the leader is reported alongside it.
Dominance assess_dominance(const BandPowers& bp, double codominance_threshold = 0.05);

}  // namespace elanora::data
