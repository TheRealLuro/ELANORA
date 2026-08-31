#include "elanora/types.hpp"

namespace elanora {

const char* band_name(Band b) {
    switch (b) {
        case Band::Delta: return "delta";
        case Band::Theta: return "theta";
        case Band::Alpha: return "alpha";
        case Band::Beta:  return "beta";
        case Band::Gamma: return "gamma";
    }
    return "unknown";
}

const char* period_name(Period p) {
    switch (p) {
        case Period::Baseline: return "baseline";
        case Period::Stimulus: return "stimulus";
        case Period::Post:     return "post";
    }
    return "unknown";
}

const char* condition_name(Condition c) {
    switch (c) {
        case Condition::Stim:          return "stim";
        case Condition::ControlJitter: return "control_jitter";
        case Condition::ControlTone:   return "control_tone";
    }
    return "unknown";
}

const char* sensor_name(SensorId s) {
    switch (s) {
        case SensorId::S1: return "S1";
        case SensorId::S2: return "S2";
        case SensorId::S3: return "S3";
        case SensorId::S4: return "S4";
    }
    return "unknown";
}

// Fixed by the Muse 2 hardware layout, in BrainFlow channel order.
const char* electrode_name(SensorId s) {
    switch (s) {
        case SensorId::S1: return "TP9";
        case SensorId::S2: return "AF7";
        case SensorId::S3: return "AF8";
        case SensorId::S4: return "TP10";
    }
    return "unknown";
}

bool parse_condition(const std::string& text, Condition& out) {
    if (text == "stim")           { out = Condition::Stim;          return true; }
    if (text == "control_jitter") { out = Condition::ControlJitter; return true; }
    if (text == "control_tone")   { out = Condition::ControlTone;   return true; }
    return false;
}

bool parse_period(const std::string& text, Period& out) {
    if (text == "baseline") { out = Period::Baseline; return true; }
    if (text == "stimulus") { out = Period::Stimulus; return true; }
    if (text == "post")     { out = Period::Post;     return true; }
    return false;
}

}  // namespace elanora
