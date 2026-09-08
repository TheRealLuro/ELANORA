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

// The shape of the amplitude envelope on a stimulus round.
//
// Two ways to deliver the same rate, and they are not equivalent stimuli. A
// gated pulse train switches the carrier hard on and off, so its envelope is a
// square wave whose spectrum contains the rate AND all its odd harmonics -- a
// 10 Hz isochronic tone also drives 30, 50 and 70 Hz. A sine envelope puts
// energy at the modulation rate and nowhere else.
//
// That difference matters for what a response means: an alpha change under a
// gated 10 Hz stimulus could be driven by the harmonics rather than by 10 Hz
// itself, and only the sine condition can separate the two. They are offered
// as separate session types so each can be learned from on its own.
enum class Envelope {
    Gated,   // isochronic: hard on/off at the rate, raised-cosine edges
    Wave     // sinusoidal amplitude modulation at the rate
};

const char* envelope_name(Envelope e);

struct StimulusDesign {
    StimMode           mode       = StimMode::Single;
    Envelope           envelope   = Envelope::Gated;
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

// The protocol every subject runs. Kept here rather than as defaults on the UI
// struct because these are experimental constants, not view preferences, and
// the analysis layer's data budget is sized for exactly these numbers.
//
// 14 points from 0.5 to 45 Hz give a ratio of 90^(1/13) = 1.414 -- half-octave
// steps. The count and the spacing are the same fact stated two ways, so a
// count of 16 would silently make the steps 1.35 and no longer half octaves,
// and would stretch the session from 18 rounds to 20.
inline constexpr double kProtocolFreqLo    = 0.5;
inline constexpr double kProtocolFreqHi    = 45.0;
inline constexpr int    kProtocolFreqCount = 14;
inline constexpr int    kProtocolJitter    = 2;
inline constexpr int    kProtocolTone      = 2;
inline constexpr int    kProtocolRounds =
    kProtocolFreqCount + kProtocolJitter + kProtocolTone;   // 18

// Geometric frequency set: `count` values from lo to hi with a constant ratio.
// Geometric rather than linear because 1->2 Hz doubles the rate while 40->41
// barely changes it, so equal Hz steps oversample the top of the range and
// starve the bottom.
std::vector<double> geometric_set(double lo, double hi, int count);

// Interleaved frequency coverage across sessions.
//
// Quarter-octave spacing over 0.5-45 Hz is 27 frequencies, which is 31 rounds
// and 62 minutes -- too long for one sitting, and a fatigued subject in round
// 28 produces worse data than no data at all.
//
// So resolution comes from sessions rather than from session length. Bank b of
// n takes every nth frequency starting at index b, which keeps each session
// near 18 rounds while n sessions together cover the full set. Interleaving
// rather than splitting the range in half matters: every bank still spans 0.5
// to 45 Hz, so a session-level shift -- different electrode placement, a
// different time of day, a worse night's sleep -- lands on the whole range
// instead of concentrating on one end and masquerading as a frequency effect.
//
// bank >= banks, or banks < 1, returns everything.
std::vector<double> frequency_bank(const std::vector<double>& all, int bank, int banks);

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
    int  relaxation   = 0;    // 1..7, 0 = unanswered
    int  alertness    = 0;    // 1..7
    int  pleasantness = 0;    // 1..7
    int  discomfort   = 0;    // 0..3

    // The manipulation check. If subjects report a steady rhythm on jittered
    // control trials as often as on stimulus trials, the jitter is not doing
    // its job and the control is worthless -- which is something only the
    // subject can tell us.
    int  rhythm = -1;         // 0 clear, 1 faint, 2 none, -1 unanswered

    // Perceived breathing change, and an optional self-count to compare
    // against the IMU estimate. The IMU is an indirect measure; a subject's
    // own count is the only independent check available on this hardware.
    int  breathing_perceived = -1;   // 0 slower, 1 same, 2 faster, -1 unanswered
    int  breaths_self_count  = -1;   // -1 = not counted

    bool jaw = false, moved = false, eyes_open = false, swallowed = false, noise = false;

    std::string note;

    // Relaxation, alertness and the rhythm check are required. Blank rows must
    // not reach the dataset, so the submit control stays disabled until they
    // are answered; everything else is genuinely optional.
    bool complete() const { return relaxation > 0 && alertness > 0 && rhythm >= 0; }
    void clear() { *this = Survey{}; }
};

}  // namespace elanora::collector
