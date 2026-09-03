#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "elanora/collector/tone.hpp"

using namespace elanora;
using namespace elanora::collector;

namespace {

constexpr int kSr = 48000;

std::vector<float> render(ToneGenerator& g, double seconds) {
    const int frames = static_cast<int>(seconds * kSr);
    std::vector<float> buf(static_cast<std::size_t>(frames) * 2u, 0.0f);
    g.render(buf.data(), frames);
    return buf;
}

// Counts gate openings from the signal envelope, which is how a listener
// perceives the rate -- not from the generator's internal state.
// Peak of |x| in short blocks, rather than a one-pole follower.
//
// A follower fast enough to resolve a 50 ms pulse also tracks the 440 Hz
// carrier and ripples across the threshold; a slow one merges the pulses. Block
// peaks separate the two timescales cleanly.
int count_pulses(const std::vector<float>& buf) {
    const int frames = static_cast<int>(buf.size() / 2);
    constexpr int kBlock = 96;              // 2 ms at 48 kHz
    int pulses = 0;
    bool on = false;
    for (int b = 0; b + kBlock <= frames; b += kBlock) {
        float peak = 0.0f;
        for (int i = 0; i < kBlock; ++i) {
            peak = std::max(peak, std::abs(buf[(b + i) * 2]));
        }
        if (!on && peak > 0.20f) { on = true; ++pulses; }
        else if (on && peak < 0.06f) { on = false; }
    }
    return pulses;
}

StimulusDesign sweep_design() {
    StimulusDesign d;
    d.mode = StimMode::Single;
    d.carrier_hz = 440.0;
    d.duty = 0.5;
    return d;
}

}  // namespace

TEST_CASE("a closed gate renders exact silence", "[tone]") {
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::Stim, 10.0, 0.0, 1);
    g.set_gate(false);
    const auto buf = render(g, 0.2);
    for (float s : buf) REQUIRE(s == 0.0f);
}

TEST_CASE("a 10 Hz round produces ten pulses per second", "[tone]") {
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::Stim, 10.0, 0.0, 1);
    g.set_gate(true);
    REQUIRE(count_pulses(render(g, 1.0)) == Catch::Approx(10).margin(1));
}

TEST_CASE("a 4 Hz round produces four pulses per second", "[tone]") {
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::Stim, 4.0, 0.0, 1);
    g.set_gate(true);
    REQUIRE(count_pulses(render(g, 1.0)) == Catch::Approx(4).margin(1));
}

TEST_CASE("output never clips, however many layers are stacked", "[tone]") {
    // Adding a layer must not make the stimulus louder, or every extra
    // frequency also raises volume and volume becomes the confound.
    StimulusDesign d;
    d.mode = StimMode::Stacked;
    d.layers = {{4.0, 1.0, true}, {10.0, 1.0, true}, {18.0, 1.0, true}, {32.0, 1.0, true}};

    ToneGenerator g(kSr);
    g.configure(d, Condition::Stim, 0.0, 0.0, 1);
    g.set_gate(true);
    for (float s : render(g, 1.0)) REQUIRE(std::abs(s) <= 1.0f);
}

TEST_CASE("all three conditions are matched for loudness", "[tone]") {
    // The controls exist to isolate rhythm. If the tone control is simply
    // louder than a stimulus round, loudness is the difference being measured
    // and the control is worthless.
    StimulusDesign d = sweep_design();

    ToneGenerator a(kSr); a.configure(d, Condition::Stim, 10.0, 0.0, 1); a.set_gate(true);
    ToneGenerator b(kSr); b.configure(d, Condition::ControlJitter, 0.0, 10.0, 7); b.set_gate(true);
    ToneGenerator c(kSr); c.configure(d, Condition::ControlTone, 0.0, 0.0, 1); c.set_gate(true);

    const auto ba = render(a, 2.0), bb = render(b, 2.0), bc = render(c, 2.0);
    const double ra = ToneGenerator::rms(ba.data(), kSr * 2);
    const double rb = ToneGenerator::rms(bb.data(), kSr * 2);
    const double rc = ToneGenerator::rms(bc.data(), kSr * 2);

    REQUIRE(rb == Catch::Approx(ra).epsilon(0.18));
    REQUIRE(rc == Catch::Approx(ra).epsilon(0.18));
}

TEST_CASE("the jitter control keeps the pulse count but loses the rate", "[tone]") {
    // Same number of pulses as a 10 Hz round, no consistent interval between
    // them. That is the whole design of the control.
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::ControlJitter, 0.0, 10.0, 42);
    g.set_gate(true);
    const auto buf = render(g, 2.0);
    const int pulses = count_pulses(buf);
    REQUIRE(pulses >= 15);
    REQUIRE(pulses <= 25);

    // Gaps must actually vary; a "jitter" control with constant spacing is
    // just a second stimulus round.
    std::vector<int> onsets;
    bool on = false;
    for (int b = 0; b + 96 <= kSr * 2; b += 96) {
        float peak = 0.0f;
        for (int i = 0; i < 96; ++i) peak = std::max(peak, std::abs(buf[(b + i) * 2]));
        if (!on && peak > 0.20f) { on = true; onsets.push_back(b); }
        else if (on && peak < 0.06f) { on = false; }
    }
    REQUIRE(onsets.size() >= 4);
    int min_gap = 1 << 30, max_gap = 0;
    for (std::size_t i = 1; i < onsets.size(); ++i) {
        const int gap = onsets[i] - onsets[i - 1];
        min_gap = std::min(min_gap, gap);
        max_gap = std::max(max_gap, gap);
    }
    REQUIRE(max_gap > min_gap * 5 / 4);
}

TEST_CASE("the continuous control has no gating at all", "[tone]") {
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::ControlTone, 0.0, 0.0, 1);
    g.set_gate(true);
    // One continuous tone: the envelope never drops back to silence, so the
    // pulse counter sees a single onset.
    REQUIRE(count_pulses(render(g, 1.0)) == 1);
}

TEST_CASE("gate edges are ramped, never stepped", "[tone]") {
    // A hard edge is a click, and a click is itself a startling stimulus --
    // it would sit in the recording as an evoked response having nothing to do
    // with the rate under test.
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::Stim, 5.0, 0.0, 1);
    g.set_gate(true);
    const auto buf = render(g, 0.5);

    float max_step = 0.0f;
    for (int i = 1; i < static_cast<int>(buf.size() / 2); ++i) {
        max_step = std::max(max_step, std::abs(buf[i * 2] - buf[(i - 1) * 2]));
    }
    // A 440 Hz carrier at 48 kHz steps by at most ~0.06 per sample between
    // adjacent samples; an unramped gate edge would jump the full amplitude.
    REQUIRE(max_step < 0.15f);
}

TEST_CASE("a stimulus round with no frequency is silent rather than undefined", "[tone]") {
    ToneGenerator g(kSr);
    g.configure(sweep_design(), Condition::Stim, 0.0, 0.0, 1);
    g.set_gate(true);
    for (float s : render(g, 0.2)) REQUIRE(s == 0.0f);
}
