#pragma once
//
// The evidence gate (Task 22).
//
// This milestone decides whether the rest of the project means anything. It
// answers two questions before any optimizer is trusted:
//
//   1. Does stimulus differ from control?   (Welch's t, FDR corrected)
//   2. Does frequency predict the response? (grouped CV against a permuted null)
//
// The system is built so that the answer can be "no". An optimizer that always
// returns a frequency is worthless if frequency turns out not to predict
// anything, so statistical evidence gates the optimizer rather than decorating
// it. A run where all 22 outcomes read NoEvidence is a valid result, not a
// failure of the software.

#include <filesystem>
#include <string>
#include <vector>

#include "elanora/types.hpp"

namespace elanora::data {

enum class Verdict { NoEvidence, Weak, Supported };

const char* verdict_name(Verdict v);

struct OutcomeEvidence {
    std::string outcome;      // "S1_d_logabs_alpha", "heart_d_bpm", ...
    std::string family;       // "brain" | "heart" | "breath"
    std::string sensor;       // "S1".."S4", empty for heart and breathing

    int n_stim    = 0;
    int n_control = 0;

    // Stimulus versus pooled controls.
    double effect_stim_vs_control = 0.0;   // Cohen's d
    double mean_stim    = 0.0;
    double mean_control = 0.0;
    double p_raw = 1.0;
    double p_fdr = 1.0;

    // Does frequency predict the response, beyond what shuffled labels give?
    double cv_r2           = 0.0;
    double cv_r2_null_mean = 0.0;
    double p_frequency     = 1.0;

    Verdict verdict = Verdict::NoEvidence;

    // Set when the outcome could not be tested at all -- too few rows, or a
    // single session so grouped validation cannot run. Distinct from "tested
    // and found nothing", and the UI must not conflate the two.
    std::string insufficient;
    bool tested() const { return insufficient.empty(); }
};

struct EvidenceOptions {
    // The plan calls for 5000. Tests override it downward; nothing else should.
    int    n_permutations = 5000;
    uint64_t seed = 20260101;

    // Per-signal exclusion thresholds. A trial with clean EEG and unusable
    // breathing still feeds the brain outcomes.
    double min_quality_eeg   = 0.5;
    double min_quality_heart = 0.5;
    double min_confidence_breath = 0.5;

    double alpha = 0.05;

    // Which response type is tested. "d" is the immediate response; the others
    // are available for the same analysis without re-recording anything.
    std::string response = "d";     // "d" | "post_d" | "recovery"
};

// Reads ml/*.csv under `root`, returns one entry per outcome in a fixed order:
// S1..S4 x delta..gamma, then heart, then breathing.
std::vector<OutcomeEvidence> analyze(const std::filesystem::path& root,
                                     const EvidenceOptions& opt = {});

// Writes analysis/evidence.csv.
void write_evidence(const std::filesystem::path& root,
                    const std::vector<OutcomeEvidence>& ev);

std::vector<OutcomeEvidence> read_evidence(const std::filesystem::path& root);

// 4 sensors x 5 bands + heart + breathing.
inline constexpr int kOutcomeCount = kSensorCount * kBandCount + 2;

struct EvidenceSummary {
    int supported = 0;
    int weak      = 0;
    int none      = 0;
    int untested  = 0;

    bool any_supported() const { return supported > 0; }
};

EvidenceSummary summarize(const std::vector<OutcomeEvidence>& ev);

}  // namespace elanora::data
