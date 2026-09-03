#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "elanora/collector/session.hpp"

using namespace elanora;
using namespace elanora::collector;

namespace {

bool is_control(const PlannedRound& r) { return r.cond != Condition::Stim; }

int count_gate_onsets(const StimulusDesign& d, double seconds, int sr) {
    int onsets = 0;
    bool prev = false;
    for (int i = 0; i < static_cast<int>(seconds * sr); ++i) {
        const bool on = d.envelope_at(static_cast<double>(i) / sr) > 0.01;
        if (on && !prev) ++onsets;
        prev = on;
    }
    return onsets;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stimulus design
// ---------------------------------------------------------------------------

TEST_CASE("single mode gates at the first enabled layer's rate", "[stimulus]") {
    StimulusDesign d;
    d.mode = StimMode::Single;
    d.layers = {{10.0, 1.0, true}, {4.0, 1.0, true}};
    REQUIRE(count_gate_onsets(d, 1.0, 4000) == 10);
}

TEST_CASE("single mode skips disabled layers", "[stimulus]") {
    StimulusDesign d;
    d.mode = StimMode::Single;
    d.layers = {{10.0, 1.0, false}, {4.0, 1.0, true}};
    REQUIRE(count_gate_onsets(d, 1.0, 4000) == 4);
}

TEST_CASE("stacked mode never exceeds a single layer's amplitude", "[stimulus]") {
    // The whole point of dividing by the layer count: adding layers must not
    // make the stimulus louder, or every extra frequency also raises volume
    // and volume becomes a confound.
    StimulusDesign d;
    d.mode = StimMode::Stacked;
    d.layers = {{4.0, 1.0, true}, {10.0, 1.0, true}, {18.0, 1.0, true}, {32.0, 1.0, true}};
    for (int i = 0; i < 8000; ++i) {
        REQUIRE(d.envelope_at(i / 4000.0) <= 1.0 + 1e-9);
    }
}

TEST_CASE("stacked mode carries every enabled layer", "[stimulus]") {
    // Two layers at different rates must produce a composite that is neither
    // constant nor equal to either layer alone.
    StimulusDesign d;
    d.mode = StimMode::Stacked;
    d.layers = {{4.0, 1.0, true}, {13.0, 1.0, true}};

    bool saw_zero = false, saw_half = false, saw_full = false;
    for (int i = 0; i < 4000; ++i) {
        const double v = d.envelope_at(i / 4000.0);
        if (v < 0.01) saw_zero = true;
        else if (std::abs(v - 0.5) < 0.01) saw_half = true;
        else if (v > 0.99) saw_full = true;
    }
    REQUIRE(saw_zero);
    REQUIRE(saw_half);   // exactly one of the two layers on
    REQUIRE(saw_full);   // both on together
}

TEST_CASE("an empty or all-disabled design is silent, not undefined", "[stimulus]") {
    StimulusDesign d;
    REQUIRE(d.envelope_at(0.3) == 0.0);
    d.layers = {{10.0, 1.0, false}};
    REQUIRE(d.envelope_at(0.3) == 0.0);
    REQUIRE(d.primary() == nullptr);
}

// ---------------------------------------------------------------------------
// Frequency set
// ---------------------------------------------------------------------------

TEST_CASE("geometric set spans the range with a constant ratio", "[schedule]") {
    const auto f = geometric_set(0.5, 45.0, 16);
    REQUIRE(f.size() == 16);
    REQUIRE(f.front() == Catch::Approx(0.5));
    REQUIRE(f.back() == Catch::Approx(45.0).margin(0.1));
    // Constant ratio, not constant step: equal Hz spacing would oversample the
    // top of the range and starve the bottom.
    const double r1 = f[1] / f[0];
    const double r2 = f[8] / f[7];
    REQUIRE(r1 == Catch::Approx(r2).epsilon(0.05));
}

TEST_CASE("geometric set is ascending and free of duplicates", "[schedule]") {
    const auto f = geometric_set(0.5, 45.0, 16);
    REQUIRE(std::is_sorted(f.begin(), f.end()));
    REQUIRE(std::adjacent_find(f.begin(), f.end()) == f.end());
}

// ---------------------------------------------------------------------------
// Schedule
// ---------------------------------------------------------------------------

TEST_CASE("a trial is 20 rounds from 16 frequencies plus 4 controls", "[schedule]") {
    const auto s = build_schedule(geometric_set(0.5, 45.0, 16), 2, 2, 84120);
    REQUIRE(s.size() == 20);
    REQUIRE(std::count_if(s.begin(), s.end(),
                          [](const PlannedRound& r) { return r.cond == Condition::Stim; }) == 16);
    REQUIRE(std::count_if(s.begin(), s.end(),
                          [](const PlannedRound& r) { return r.cond == Condition::ControlJitter; }) == 2);
    REQUIRE(std::count_if(s.begin(), s.end(),
                          [](const PlannedRound& r) { return r.cond == Condition::ControlTone; }) == 2);
}

TEST_CASE("controls are never first, last, or adjacent", "[schedule]") {
    // Clustered controls confound the control condition with time-in-session,
    // which is the exact thing the controls exist to rule out.
    for (uint64_t seed : {1ull, 42ull, 84120ull, 999999ull}) {
        const auto s = build_schedule(geometric_set(0.5, 45.0, 16), 2, 2, seed);
        REQUIRE_FALSE(is_control(s.front()));
        REQUIRE_FALSE(is_control(s.back()));
        for (std::size_t i = 1; i < s.size(); ++i) {
            REQUIRE_FALSE((is_control(s[i]) && is_control(s[i - 1])));
        }
    }
}

TEST_CASE("the same seed reproduces the same order", "[schedule]") {
    // The seed is written to the session record, so a trial must be exactly
    // reconstructible from it months later.
    const auto a = build_schedule(geometric_set(0.5, 45.0, 16), 2, 2, 7);
    const auto b = build_schedule(geometric_set(0.5, 45.0, 16), 2, 2, 7);
    const auto c = build_schedule(geometric_set(0.5, 45.0, 16), 2, 2, 8);
    REQUIRE(a == b);
    REQUIRE(a != c);
}

TEST_CASE("every stimulus frequency appears exactly once", "[schedule]") {
    const auto freqs = geometric_set(0.5, 45.0, 16);
    const auto s = build_schedule(freqs, 2, 2, 84120);
    for (double f : freqs) {
        REQUIRE(std::count_if(s.begin(), s.end(), [f](const PlannedRound& r) {
            return r.cond == Condition::Stim && r.hz == f;
        }) == 1);
    }
}

// ---------------------------------------------------------------------------
// Trial runner
// ---------------------------------------------------------------------------

TEST_CASE("phases advance in order and the round ends after rest", "[trial]") {
    TrialRunner t;
    Durations d{2.0, 2.0, 2.0, 2.0};
    t.start(build_schedule(geometric_set(1.0, 20.0, 4), 0, 0, 1), d);

    REQUIRE(t.phase() == Phase::Baseline);
    for (int i = 0; i < 21; ++i) t.tick(0.1);
    REQUIRE(t.phase() == Phase::Stimulus);
    for (int i = 0; i < 21; ++i) t.tick(0.1);
    REQUIRE(t.phase() == Phase::Post);
    for (int i = 0; i < 21; ++i) t.tick(0.1);
    REQUIRE(t.phase() == Phase::Rest);

    bool ended = false;
    for (int i = 0; i < 21 && !ended; ++i) ended = t.tick(0.1);
    REQUIRE(ended);
    REQUIRE(t.awaiting_survey());
}

TEST_CASE("the trial halts while the survey is open", "[trial]") {
    // If the clock kept running the next round would start recording while the
    // subject is still answering, and its baseline would be contaminated.
    TrialRunner t;
    Durations d{1.0, 1.0, 1.0, 1.0};
    t.start(build_schedule(geometric_set(1.0, 20.0, 4), 0, 0, 1), d);
    for (int i = 0; i < 100; ++i) t.tick(0.1);
    REQUIRE(t.awaiting_survey());

    const int round_before = t.round_index();
    for (int i = 0; i < 200; ++i) t.tick(0.1);
    REQUIRE(t.round_index() == round_before);
    REQUIRE(t.awaiting_survey());
}

TEST_CASE("submitting the survey starts the next round at baseline", "[trial]") {
    TrialRunner t;
    Durations d{1.0, 1.0, 1.0, 1.0};
    t.start(build_schedule(geometric_set(1.0, 20.0, 4), 0, 0, 1), d);
    for (int i = 0; i < 100; ++i) t.tick(0.1);
    t.advance_round();
    REQUIRE(t.round_index() == 1);
    REQUIRE(t.phase() == Phase::Baseline);
    REQUIRE_FALSE(t.awaiting_survey());
}

TEST_CASE("the trial finishes after the last round's survey", "[trial]") {
    TrialRunner t;
    Durations d{0.5, 0.5, 0.5, 0.5};
    t.start(build_schedule(geometric_set(1.0, 20.0, 3), 0, 0, 1), d);
    for (int r = 0; r < 3; ++r) {
        for (int i = 0; i < 40; ++i) t.tick(0.1);
        REQUIRE(t.awaiting_survey());
        t.advance_round();
    }
    REQUIRE(t.finished());
    REQUIRE_FALSE(t.running());
}

TEST_CASE("the gate is open only during the stimulus phase", "[trial]") {
    // The one moment anything is audible. If this were true during rest the
    // post-stimulus window would not be silent and the design would be void.
    TrialRunner t;
    Durations d{1.0, 1.0, 1.0, 1.0};
    t.start(build_schedule(geometric_set(1.0, 20.0, 4), 0, 0, 1), d);

    REQUIRE_FALSE(t.gate_open());
    for (int i = 0; i < 11; ++i) t.tick(0.1);
    REQUIRE(t.gate_open());
    for (int i = 0; i < 11; ++i) t.tick(0.1);
    REQUIRE_FALSE(t.gate_open());
}

TEST_CASE("aborting stops the clock", "[trial]") {
    TrialRunner t;
    Durations d{1.0, 1.0, 1.0, 1.0};
    t.start(build_schedule(geometric_set(1.0, 20.0, 4), 0, 0, 1), d);
    t.abort();
    REQUIRE_FALSE(t.running());
    REQUIRE_FALSE(t.tick(0.1));
}

// ---------------------------------------------------------------------------
// Survey
// ---------------------------------------------------------------------------

TEST_CASE("a survey is incomplete until the first three are answered", "[survey]") {
    // Blank rows must not reach the dataset, so submit stays disabled.
    Survey s;
    REQUIRE_FALSE(s.complete());
    s.relaxation = 5;  REQUIRE_FALSE(s.complete());
    s.alertness  = 3;  REQUIRE_FALSE(s.complete());
    s.rhythm     = 0;  REQUIRE(s.complete());
}

TEST_CASE("artifact checkboxes are optional", "[survey]") {
    Survey s;
    s.relaxation = 4; s.alertness = 4; s.rhythm = 2;
    REQUIRE(s.complete());
    s.jaw = true;
    REQUIRE(s.complete());
}

TEST_CASE("14 points from 0.5 to 45 Hz are half-octave steps", "[session][protocol]") {
    // A Global Constraint, pinned here because the count is a plain int in a
    // settings struct and therefore easy to nudge: 14 points across this range
    // give a ratio of sqrt(2), which is what "half-octave" means. Changing the
    // count silently changes the spacing, the round count, and the data budget.
    REQUIRE(kProtocolFreqCount == 14);
    REQUIRE(kProtocolRounds == 18);

    const auto f = geometric_set(kProtocolFreqLo, kProtocolFreqHi, kProtocolFreqCount);
    REQUIRE(f.size() == 14);
    REQUIRE(f.front() == Catch::Approx(kProtocolFreqLo));
    REQUIRE(f.back() == Catch::Approx(kProtocolFreqHi));
    // Asserted against the unrounded law rather than on consecutive ratios.
    // Values are rounded to 0.1 Hz for display, and at the bottom of the range
    // that rounding is worth ~1% of the step (0.707 shows as 0.7), which would
    // make a tight ratio test fail on the presentation rather than the spacing.
    const double ratio = std::pow(kProtocolFreqHi / kProtocolFreqLo,
                                  1.0 / (kProtocolFreqCount - 1));
    REQUIRE(ratio == Catch::Approx(std::sqrt(2.0)).epsilon(0.01));
    for (std::size_t i = 0; i < f.size(); ++i) {
        const double ideal = kProtocolFreqLo * std::pow(ratio, static_cast<double>(i));
        REQUIRE(f[i] == Catch::Approx(ideal).margin(0.05));
    }

    // 14 stimulus rounds plus 2 jitter and 2 tone controls is the 18-round,
    // 36-minute session the data budget is sized for.
    REQUIRE(build_schedule(f, kProtocolJitter, kProtocolTone, 84120).size()
            == static_cast<std::size_t>(kProtocolRounds));
}
