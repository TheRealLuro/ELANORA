#pragma once
//
// Core vocabulary shared by all four ELANORA applications.
//
// Everything here is intentionally small and dependency-free: the four apps
// disagree about almost everything else, but they must agree exactly on what a
// trial, a period, a condition and a band are.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace elanora {

// ---------------------------------------------------------------------------
// Trial anatomy
// ---------------------------------------------------------------------------

// One 90-second trial is cut into three equal 30-second periods. Equal length
// matters: spectral estimates from windows of different duration are not
// directly comparable, so baseline/stimulus/post must all be the same size.
enum class Period { Baseline, Stimulus, Post };
inline constexpr int kPeriodCount = 3;

// Every trial carries one of these. There is no unlabeled trial.
//   Stim          - regular isochronic gating at the target rate
//   ControlJitter - same pulse count, randomized intervals: isolates rhythmicity
//   ControlTone   - unmodulated carrier: isolates the presence of sound
enum class Condition { Stim, ControlJitter, ControlTone };

// Muse 2 electrode positions. This mapping is fixed by the hardware and must
// never be reordered -- S1..S4 appear in every CSV ever written.
enum class SensorId { S1, S2, S3, S4 };
inline constexpr int kSensorCount = 4;

enum class Band { Delta, Theta, Alpha, Beta, Gamma };
inline constexpr int kBandCount = 5;

// Band edges in Hz. Gamma stops at 45, not 50, to stay clear of 60 Hz mains
// noise and the skirt of the anti-alias filter.
inline constexpr std::array<std::array<double, 2>, kBandCount> kBandEdges{{
    {1.0, 4.0},    // Delta
    {4.0, 8.0},    // Theta
    {8.0, 13.0},   // Alpha
    {13.0, 30.0},  // Beta
    {30.0, 45.0},  // Gamma
}};

inline constexpr double kAnalysisLowHz  = 1.0;
inline constexpr double kAnalysisHighHz = 45.0;

// ---------------------------------------------------------------------------
// Name lookups. Used for CSV column construction, so the spellings are load
// bearing -- changing one invalidates every dataset already on disk.
// ---------------------------------------------------------------------------

const char* band_name(Band b);
const char* period_name(Period p);
const char* condition_name(Condition c);
const char* sensor_name(SensorId s);     // "S1".."S4"
const char* electrode_name(SensorId s);  // "TP9", "AF7", "AF8", "TP10"

bool parse_condition(const std::string& text, Condition& out);
bool parse_period(const std::string& text, Period& out);

inline constexpr int index_of(Band b)     { return static_cast<int>(b); }
inline constexpr int index_of(SensorId s) { return static_cast<int>(s); }
inline constexpr int index_of(Period p)   { return static_cast<int>(p); }

// ---------------------------------------------------------------------------
// Features
// ---------------------------------------------------------------------------

// Band power for one sensor over one period, in both representations.
//
// Both are stored deliberately. Relative power is blind to amplitude changes
// and is compositional (the five values sum to 1, so their deltas sum to zero
// by construction). Absolute power escapes that constraint but is contaminated
// by electrode impedance, which drifts between and within sessions. Neither
// alone is sufficient, so nothing is discarded at extraction time.
struct BandPowers {
    std::array<double, kBandCount> abs{};  // raw power per band
    std::array<double, kBandCount> rel{};  // abs / total, sums to 1
    double total = 0.0;                    // total power over 1-45 Hz

    double abs_of(Band b) const { return abs[index_of(b)]; }
    double rel_of(Band b) const { return rel[index_of(b)]; }
};

// ---------------------------------------------------------------------------
// Trial metadata
// ---------------------------------------------------------------------------

struct TrialMeta {
    std::string trial_id;
    std::string session_id;
    std::string subject_id;
    int         round_index = 0;
    Condition   condition   = Condition::Stim;

    // Meaningful only when condition == Stim.
    double frequency_hz = 0.0;

    // Meaningful only when condition == ControlJitter: the mean rate the
    // jittered pulses were drawn around, recorded so the control can be
    // matched against the stimulus trials it is meant to control for.
    double jitter_mean_hz = 0.0;

    double started_at = 0.0;  // board-clock epoch seconds
    std::string notes;
};

// Session-level configuration, written once per session.
struct SessionMeta {
    std::string session_id;
    std::string subject_id;
    std::string date;
    std::string device_serial;
    double   carrier_hz = 440.0;
    double   duty_cycle = 0.5;
    double   baseline_s = 30.0;
    double   stimulus_s = 30.0;
    double   post_s     = 30.0;
    double   rest_s     = 30.0;
    uint64_t order_seed = 0;
    std::string freq_set;
    std::string notes;
};

}  // namespace elanora
