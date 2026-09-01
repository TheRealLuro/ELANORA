#include "elanora/collector/session.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace elanora::collector {

// ---------------------------------------------------------------------------
// Stimulus design
// ---------------------------------------------------------------------------

int StimulusDesign::enabled_count() const {
    int n = 0;
    for (const auto& l : layers) if (l.enabled) ++n;
    return n;
}

const Layer* StimulusDesign::primary() const {
    for (const auto& l : layers) if (l.enabled) return &l;
    return nullptr;
}

double StimulusDesign::envelope_at(double t) const {
    if (mode == StimMode::Single) {
        const Layer* l = primary();
        if (l == nullptr) return 0.0;
        const double ph = std::fmod(t * l->hz, 1.0);
        return (ph < duty ? 1.0 : 0.0) * l->amp;
    }

    // Stacked: sum the gates and divide by the number of contributors, so the
    // composite can never exceed the amplitude of a single layer no matter how
    // many are added. Without this, four layers at full amplitude clip.
    const int n = enabled_count();
    if (n == 0) return 0.0;
    double sum = 0.0;
    for (const auto& l : layers) {
        if (!l.enabled) continue;
        const double ph = std::fmod(t * l.hz, 1.0);
        sum += (ph < duty ? 1.0 : 0.0) * l.amp;
    }
    return sum / static_cast<double>(n);
}

// ---------------------------------------------------------------------------
// Schedule
// ---------------------------------------------------------------------------

std::vector<double> geometric_set(double lo, double hi, int count) {
    std::vector<double> out;
    if (count < 1 || lo <= 0.0 || hi <= lo) return out;
    if (count == 1) { out.push_back(lo); return out; }

    const double ratio = std::pow(hi / lo, 1.0 / (count - 1));
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        // Rounded to 0.1 Hz: the extra precision is not audible, not
        // reproducible on the hardware, and clutters every label it appears in.
        out.push_back(std::round(lo * std::pow(ratio, i) * 10.0) / 10.0);
    }
    return out;
}

std::vector<PlannedRound> build_schedule(const std::vector<double>& freqs,
                                         int n_jitter, int n_tone, uint64_t seed) {
    std::vector<PlannedRound> list;
    list.reserve(freqs.size() + static_cast<std::size_t>(n_jitter + n_tone));
    for (double f : freqs) list.push_back(PlannedRound{Condition::Stim, f, 0.0});

    // Each jitter control takes its mean rate from the stimulus set, so it is
    // matched to the rounds it is meant to control for rather than arbitrary.
    for (int i = 0; i < n_jitter; ++i) {
        const double mean = freqs.empty()
            ? 10.0
            : freqs[(freqs.size() / (static_cast<std::size_t>(n_jitter) + 1u)) *
                    static_cast<std::size_t>(i + 1) % freqs.size()];
        list.push_back(PlannedRound{Condition::ControlJitter, 0.0, mean});
    }
    for (int i = 0; i < n_tone; ++i) {
        list.push_back(PlannedRound{Condition::ControlTone, 0.0, 0.0});
    }
    if (list.size() < 3) return list;

    auto is_control = [](const PlannedRound& r) { return r.cond != Condition::Stim; };
    auto acceptable = [&](const std::vector<PlannedRound>& v) {
        if (is_control(v.front()) || is_control(v.back())) return false;
        for (std::size_t i = 1; i < v.size(); ++i) {
            if (is_control(v[i]) && is_control(v[i - 1])) return false;
        }
        return true;
    };

    std::mt19937_64 rng(seed);
    for (int attempt = 0; attempt < 500; ++attempt) {
        std::shuffle(list.begin(), list.end(), rng);
        if (acceptable(list)) break;
    }
    return list;
}

// ---------------------------------------------------------------------------
// Trial state machine
// ---------------------------------------------------------------------------

double Durations::of(Phase p) const {
    switch (p) {
        case Phase::Baseline: return baseline;
        case Phase::Stimulus: return stimulus;
        case Phase::Post:     return post;
        case Phase::Rest:     return rest;
    }
    return 0.0;
}

void TrialRunner::start(std::vector<PlannedRound> schedule, Durations d) {
    schedule_ = std::move(schedule);
    d_ = d;
    phase_ = Phase::Baseline;
    phase_t_ = 0.0;
    round_ = 0;
    running_ = !schedule_.empty();
    finished_ = false;
    awaiting_survey_ = false;
}

bool TrialRunner::tick(double dt) {
    if (!running_ || awaiting_survey_ || finished_) return false;

    phase_t_ += dt;
    if (phase_t_ < d_.of(phase_)) return false;

    phase_t_ = 0.0;
    switch (phase_) {
        case Phase::Baseline: phase_ = Phase::Stimulus; return false;
        case Phase::Stimulus: phase_ = Phase::Post;     return false;
        case Phase::Post:     phase_ = Phase::Rest;     return false;
        case Phase::Rest:
            // The round is over. Hold here until the survey is submitted --
            // the trial must not run on while the subject is still answering.
            awaiting_survey_ = true;
            phase_t_ = d_.rest;
            return true;
    }
    return false;
}

void TrialRunner::advance_round() {
    if (!awaiting_survey_) return;
    awaiting_survey_ = false;
    if (round_ + 1 >= static_cast<int>(schedule_.size())) {
        finished_ = true;
        running_ = false;
        return;
    }
    ++round_;
    phase_ = Phase::Baseline;
    phase_t_ = 0.0;
}

void TrialRunner::abort() {
    running_ = false;
    awaiting_survey_ = false;
    finished_ = false;
}

const PlannedRound& TrialRunner::current() const {
    static const PlannedRound kNone{};
    if (schedule_.empty()) return kNone;
    return schedule_[static_cast<std::size_t>(
        std::min(round_, static_cast<int>(schedule_.size()) - 1))];
}

double TrialRunner::trial_elapsed() const {
    double done = d_.round_total() * static_cast<double>(round_);
    switch (phase_) {
        case Phase::Rest:     done += d_.post;      [[fallthrough]];
        case Phase::Post:     done += d_.stimulus;  [[fallthrough]];
        case Phase::Stimulus: done += d_.baseline;  [[fallthrough]];
        case Phase::Baseline: break;
    }
    return done + phase_t_;
}

}  // namespace elanora::collector
