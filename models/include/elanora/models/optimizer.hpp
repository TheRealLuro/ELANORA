#pragma once
//
// Forward prediction (Task 29) and inverse optimization (Task 30).
//
// Forward: given a frequency and a person's current state, what does every
// model predict happens?
//
// Inverse: given a desired state, which frequency is most likely to produce it?
//
// The inverse direction is the one that needs guarding. Three separate
// mechanisms exist so the system can answer "no suitable frequency":
//
//   1. The evidence gate. A goal on an outcome that Milestone 4 found no
//      evidence for is refused outright. Optimising over a model of noise
//      produces a confident recommendation with nothing behind it.
//   2. A hard clamp to the trained frequency range. GP variance saturates, so
//      uncertainty alone cannot keep the search from wandering somewhere
//      nothing was ever measured.
//   3. An effect-size floor at twice the control-trial standard deviation. A
//      predicted change smaller than the noise seen with no rhythmic stimulus
//      is not an effect, however well the score ranks it.
//
// All wording produced here is associational. Control comparison and repetition
// strengthen an inference; they do not license the word "cause".

#include <array>
#include <string>
#include <vector>

#include "elanora/data/evidence.hpp"
#include "elanora/models/model_registry.hpp"
#include "elanora/types.hpp"

namespace elanora::models {

// A person's state before the stimulus. Brain values are log absolute band
// power, matching the models' baseline inputs.
struct BaselineState {
    std::array<std::array<double, kBandCount>, kSensorCount> brain_logabs{};
    double bpm       = 60.0;
    double breathing = 12.0;

    // A neutral state for exploring the models without a real recording.
    //
    // Prefer from_registry(): a baseline far outside what the models were
    // trained on is not merely inaccurate, it is silently uninformative. An
    // RBF kernel evaluated 35 standard deviations from every training point
    // underflows to zero, so the GP returns its prior mean -- which reads as a
    // confident prediction of "no change" rather than as "I have no idea".
    static BaselineState neutral();

    // The mean baseline of the training data, per model. This is the right
    // default for exploring a trained registry.
    static BaselineState from_registry(const ModelRegistry& reg);
};

// Beyond this many standard deviations the prediction carries no information
// from the training data. Four is generous for a quantity that should be a
// physiological measurement of the same kind the models were trained on.
inline constexpr double kBaselineZLimit = 4.0;

struct Prediction {
    std::array<std::array<double, kBandCount>, kSensorCount> d_bands{};
    std::array<std::array<double, kBandCount>, kSensorCount> d_bands_sd{};
    double d_bpm = 0.0, d_bpm_sd = 0.0;
    double d_breathing = 0.0, d_breathing_sd = 0.0;

    double frequency_hz = 0.0;

    // How far the supplied baseline sits from the training data, in standard
    // deviations of the training baselines, taken over every output consulted.
    // Large values mean the models are extrapolating on the baseline axis, and
    // a GP that far out returns its prior mean rather than a real estimate.
    double max_baseline_z = 0.0;
    bool baseline_in_range() const { return max_baseline_z <= kBaselineZLimit; }
};

// Every one of the six models evaluates the same candidate frequency; their
// outputs together are the predicted response.
Prediction predict(const ModelRegistry& reg, double frequency_hz, const BaselineState& b);

// ---------------------------------------------------------------------------
// Inverse
// ---------------------------------------------------------------------------

enum class Goal { Ignore, Increase, Decrease, Target };

const char* goal_name(Goal g);

struct BandGoal {
    Goal   goal   = Goal::Ignore;
    double target = 0.0;    // used only by Goal::Target
    double weight = 1.0;
};

struct DesiredState {
    std::array<std::array<BandGoal, kBandCount>, kSensorCount> brain{};
    BandGoal heart;
    BandGoal breathing;

    double uncertainty_penalty   = 0.5;
    double extrapolation_penalty = 1.0;
    double min_score             = 0.35;

    // Enforce the effect-size floor. Off only for exploration; a real
    // recommendation should never skip it.
    bool require_effect_floor = true;

    bool any_goal_set() const;
};

struct Candidate {
    double frequency_hz = 0.0;
    double score = 0.0;
    double goal_match = 0.0;
    double uncertainty_cost = 0.0;
    double extrapolation_cost = 0.0;
    bool   clears_effect_floor = false;
    Prediction pred;
};

struct OptimizerResult {
    bool found = false;
    std::string reason;                 // why not, when found is false
    std::vector<Candidate> ranked;      // best first; may be non-empty even when refused

    // Outcomes the request touched that have no supporting evidence.
    std::vector<std::string> unsupported_outcomes;
};

// `evidence` gates the goals. Pass the output of data::analyze(), or the
// contents of analysis/evidence.csv.
OptimizerResult optimize(const ModelRegistry& reg,
                         const std::vector<elanora::data::OutcomeEvidence>& evidence,
                         const DesiredState& desired,
                         const BaselineState& baseline);

// The outcome name an evidence row would carry for a given model output, so a
// goal can be matched to its verdict.
std::string evidence_key(ModelId m, int output_index, const std::string& response);

}  // namespace elanora::models
