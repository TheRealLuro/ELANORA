#pragma once
//
// Collector logic: stimulus design, round scheduling, and the trial state
// machine. Pure and testable -- no ImGui, no audio device, no BrainFlow.
//
// The experiment shape this implements:
//
//     Trial  ->  Round 1..20  ->  self-report
//
// Each round is baseline / stimulus / post / rest, and the survey opens by
// itself when the round ends. The operator starts the trial and does nothing
// else until it finishes.

#include <cstdint>
#include <string>
#include <vector>

#include "elanora/types.hpp"

namespace elanora::collector {

// ---------------------------------------------------------------------------
// Stimulus design
// ---------------------------------------------------------------------------

// One frequency in the stimulus. Several may sound at once in Stacked mode.
struct Layer {
    double hz      = 10.0;
    double amp     = 1.0;    // 0..1
    bool   enabled = true;
};

enum class StimMode {
    Single,   // one frequency per round; the trial sweeps the list
    Stacked   // every enabled layer sounds together in the same round
};

struct StimulusDesign {
    StimMode           mode       = StimMode::Single;
    std::vector<Layer> layers;
    double             carrier_hz = 440.0;
    double             duty       = 0.5;

    // Gate envelope at time t seconds, in 0..1.
    //
    // Stacked sums the enabled layers and divides by their count, so adding a
    // layer can never clip -- the same normalisation the audio path uses.
    // Single plays the first enabled layer alone.
    double envelope_at(double t) const;

    int enabled_count() const;

    // The layer a Single-mode round would play, or nullptr if none are on.
    const Layer* primary() const;
};

// ---------------------------------------------------------------------------
// Round schedule
// ---------------------------------------------------------------------------

struct PlannedRound {
    Condition cond           = Condition::Stim;
    double    hz             = 0.0;   // meaningful when cond == Stim
    double    jitter_mean_hz = 0.0;   // meaningful when cond == ControlJitter

    bool operator==(const PlannedRound& o) const {
        return cond == o.cond && hz == o.hz && jitter_mean_hz == o.jitter_mean_hz;
    }
};

// Geometric frequency set: `count` values from lo to hi with a constant ratio.
// Geometric rather than linear because 1->2 Hz doubles the rate while 40->41
// barely changes it, so equal Hz steps oversample the top of the range and
// starve the bottom.
std::vector<double> geometric_set(double lo, double hi, int count);

// Builds the round order for one trial.
//
// Randomised from a stored seed, under two constraints: no control round may
// be first or last, and two controls may never be adjacent. Clustered controls
// confound the control condition with time-in-session, which is the thing the
// controls exist to rule out.
std::vector<PlannedRound> build_schedule(const std::vector<double>& freqs,
                                         int n_jitter, int n_tone, uint64_t seed);

// ---------------------------------------------------------------------------
// Trial state machine
// ---------------------------------------------------------------------------

enum class Phase { Baseline, Stimulus, Post, Rest };

struct Durations {
    double baseline = 30.0;
    double stimulus = 30.0;
    double post     = 30.0;
    double rest     = 30.0;

    double of(Phase p) const;
    double round_total() const { return baseline + stimulus + post + rest; }
};

class TrialRunner {
public:
    void start(std::vector<PlannedRound> schedule, Durations d);

    // Advances by dt. Returns true on the frame a round completes, which is
    // when the caller should open the survey.
    bool tick(double dt);

    // Called after the survey is submitted. Moves to the next round, or
    // finishes the trial.
    void advance_round();

    void abort();

    bool  running() const { return running_; }
    bool  finished() const { return finished_; }
    bool  awaiting_survey() const { return awaiting_survey_; }
    Phase phase() const { return phase_; }
    double phase_elapsed() const { return phase_t_; }
    double phase_remaining(const Durations& d) const { return d.of(phase_) - phase_t_; }
    int    round_index() const { return round_; }
    int    round_count() const { return static_cast<int>(schedule_.size()); }
    const std::vector<PlannedRound>& schedule() const { return schedule_; }
    const PlannedRound& current() const;

    // True only while the gate should be open, which is the one moment the
    // audio engine plays anything.
    bool gate_open() const { return running_ && !awaiting_survey_ && phase_ == Phase::Stimulus; }

    double trial_elapsed() const;
    double trial_total(const Durations& d) const {
        return d.round_total() * static_cast<double>(schedule_.size());
    }

private:
    std::vector<PlannedRound> schedule_;
    Durations d_{};
    Phase  phase_ = Phase::Baseline;
    double phase_t_ = 0.0;
    int    round_ = 0;
    bool   running_ = false;
    bool   finished_ = false;
    bool   awaiting_survey_ = false;
};

// ---------------------------------------------------------------------------
// Self-report
// ---------------------------------------------------------------------------

struct Survey {
    int  relaxation = 0;      // 1..7, 0 = unanswered
    int  alertness  = 0;      // 1..7
    int  rhythm     = -1;     // 0 clear, 1 faint, 2 none, -1 unanswered
    bool jaw = false, moved = false, eyes_open = false, swallowed = false, noise = false;

    // The first three are required. Blank rows must not reach the dataset, so
    // the submit control stays disabled until they are answered.
    bool complete() const { return relaxation > 0 && alertness > 0 && rhythm >= 0; }
    void clear() { *this = Survey{}; }
};

}  // namespace elanora::collector
