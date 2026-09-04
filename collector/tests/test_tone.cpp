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

TEST_CASE("every stimulus rate is equally loud", "[tone][loudness]") {
    // The confound this guards against is subtle and was real: the 4 ms
    // follower that rounds gate edges removes more energy the more edges there
    // are, so RMS fell from 0.2495 at 0.5 Hz to 0.2065 at 45 Hz -- the top of
    // the sweep was 17% quieter than the bottom.
    //
    // Loudness covarying with stimulus frequency is exactly the confound the
    // three-condition RMS match exists to eliminate, arriving through the back
    // door. It would not have been caught by that check, which compares stim
    // against control at ONE rate rather than rates against each other.
    const int sr = 48000;
    const auto freqs = geometric_set(kProtocolFreqLo, kProtocolFreqHi, kProtocolFreqCount);

    std::vector<double> levels;
    for (double hz : freqs) {
        StimulusDesign d;
        d.mode = StimMode::Single;
        d.carrier_hz = 440.0;
        d.duty = 0.5;
        d.layers.push_back(Layer{hz, 1.0, true});

        ToneGenerator g(sr);
        g.configure(d, Condition::Stim, hz, 0.0, 1);
        g.set_gate(true);
        // Eight seconds, so even 0.5 Hz contributes whole cycles.
        const int frames = sr * 8;
        std::vector<float> buf(static_cast<std::size_t>(frames) * 2, 0.0f);
        g.render(buf.data(), frames);
        levels.push_back(ToneGenerator::rms(buf.data(), frames));
    }

    const double lo = *std::min_element(levels.begin(), levels.end());
    const double hi = *std::max_element(levels.begin(), levels.end());
    INFO("quietest " << lo << " loudest " << hi << " spread " << (hi / lo - 1.0) * 100 << "%");
    REQUIRE(hi / lo < 1.05);
}

TEST_CASE("a wave round matches a gated round in loudness", "[tone][loudness]") {
    // The two envelopes are meant to differ in shape and in nothing else. If
    // the wave condition were louder, any difference between them would be
    // confounded with volume rather than telling us about envelope shape.
    const int sr = 48000;
    auto level = [&](Envelope env, double hz) {
        StimulusDesign d;
        d.mode = StimMode::Single;
        d.envelope = env;
        d.carrier_hz = 440.0;
        d.duty = 0.5;
        d.layers.push_back(Layer{hz, 1.0, true});
        ToneGenerator g(sr);
        g.configure(d, Condition::Stim, hz, 0.0, 1);
        g.set_gate(true);
        std::vector<float> buf(static_cast<std::size_t>(sr) * 2 * 4, 0.0f);
        g.render(buf.data(), sr * 4);
        return ToneGenerator::rms(buf.data(), sr * 4);
    };

    for (double hz : {1.0, 10.0, 45.0}) {
        const double gated = level(Envelope::Gated, hz);
        const double wave = level(Envelope::Wave, hz);
        INFO("at " << hz << " Hz: gated " << gated << " wave " << wave);
        REQUIRE(wave == Catch::Approx(gated).epsilon(0.05));
    }
}

TEST_CASE("a wave envelope carries no harmonics of its rate", "[tone][envelope]") {
    // The reason both envelopes exist. A gated train's envelope is a square
    // wave, so a 10 Hz isochronic tone also drives 30, 50 and 70 Hz, and an
    // alpha response to it cannot be attributed to 10 Hz alone. A sine
    // envelope puts energy at the rate and nowhere else, which is what makes
    // the two separable.
    const int sr = 4800;
    auto envelope_at_3f = [&](Envelope env) {
        StimulusDesign d;
        d.mode = StimMode::Single;
        d.envelope = env;
        d.carrier_hz = 440.0;
        d.duty = 0.5;
        d.layers.push_back(Layer{10.0, 1.0, true});
        ToneGenerator g(sr);
        g.configure(d, Condition::Stim, 10.0, 0.0, 1);
        g.set_gate(true);
        std::vector<float> buf(static_cast<std::size_t>(sr) * 2, 0.0f);
        g.render(buf.data(), sr);

        // Rectify to recover the envelope, then measure the 30 Hz component
        // relative to the 10 Hz one.
        auto power_at = [&](double f) {
            double re = 0.0, im = 0.0;
            for (int i = 0; i < sr; ++i) {
                const double e = std::abs(static_cast<double>(buf[i * 2]));
                const double a = 2.0 * 3.14159265358979 * f * i / sr;
                re += e * std::cos(a);
                im += e * std::sin(a);
            }
            return (re * re + im * im) / (static_cast<double>(sr) * sr);
        };
        return power_at(30.0) / std::max(power_at(10.0), 1e-12);
    };

    const double gated_ratio = envelope_at_3f(Envelope::Gated);
    const double wave_ratio = envelope_at_3f(Envelope::Wave);
    INFO("third-harmonic ratio: gated " << gated_ratio << " wave " << wave_ratio);
    REQUIRE(wave_ratio < gated_ratio * 0.1);
}
