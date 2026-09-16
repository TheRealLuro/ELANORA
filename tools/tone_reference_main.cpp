// Emits reference values from the C++ tone generator, for the browser port to
// be checked against.
//
//   tone_reference > web/fixtures/tone_reference.json
//
// The phone makes the sound now, so tone.js is a second implementation of the
// stimulus. "It sounds about right" is not good enough: the three conditions
// being RMS-matched IS the control, and a port that drifts would change the
// experiment without changing anything visible. This pins the browser to the
// desktop numerically, and re-running it after any edit to tone.cpp makes the
// JavaScript test fail if the two have diverged.

#include <cstdio>
#include <vector>

#include "elanora/collector/tone.hpp"

using namespace elanora;
using namespace elanora::collector;

namespace {

constexpr int kSr = 48000;
constexpr int kFrames = kSr;   // one second

// Sampled rather than dumped whole: a handful of points spread across the
// second catches a phase or envelope difference, and the file stays readable.
constexpr int kProbes[] = {0, 100, 1000, 5000, 12000, 24000, 36000, 47999};

std::vector<float> render(Condition cond, double hz, double jitter_hz,
                          uint64_t seed, Envelope env = Envelope::Gated) {
    StimulusDesign design;
    design.mode = StimMode::Single;
    design.envelope = env;
    design.carrier_hz = 440.0;
    design.duty = 0.5;
    design.layers.push_back(Layer{hz, 1.0, true});

    ToneGenerator g(kSr);
    g.configure(design, cond, hz, jitter_hz, seed);
    g.set_gate(true);

    std::vector<float> buf(static_cast<std::size_t>(kFrames) * 2, 0.0f);
    g.render(buf.data(), kFrames);
    return buf;
}

// Left channel only; the generator writes the same value to both.
double rms_of(const std::vector<float>& buf) {
    return ToneGenerator::rms(buf.data(), kFrames);
}

void emit(const char* name, const std::vector<float>& buf, bool last) {
    std::printf("  \"%s\": { \"rms\": %.9f, \"samples\": [", name, rms_of(buf));
    for (std::size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        if (i) std::printf(", ");
        std::printf("%.9f", static_cast<double>(buf[static_cast<std::size_t>(kProbes[i]) * 2]));
    }
    std::printf("] }%s\n", last ? "" : ",");
}

}  // namespace

int main() {
    std::printf("{\n");
    std::printf("  \"sample_rate\": %d,\n", kSr);
    std::printf("  \"frames\": %d,\n", kFrames);
    std::printf("  \"probes\": [");
    for (std::size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); ++i) {
        std::printf("%s%d", i ? ", " : "", kProbes[i]);
    }
    std::printf("],\n");

    emit("stim_10hz", render(Condition::Stim, 10.0, 0.0, 1), false);
    emit("control_jitter_10hz", render(Condition::ControlJitter, 0.0, 10.0, 7), false);
    emit("control_tone", render(Condition::ControlTone, 0.0, 0.0, 1), false);
    emit("wave_10hz", render(Condition::Stim, 10.0, 0.0, 1, Envelope::Wave), false);
    emit("wave_45hz", render(Condition::Stim, 45.0, 0.0, 1, Envelope::Wave), false);
    emit("swell_10hz", render(Condition::Stim, 10.0, 0.0, 1, Envelope::Swell), true);

    std::printf("}\n");
    return 0;
}
