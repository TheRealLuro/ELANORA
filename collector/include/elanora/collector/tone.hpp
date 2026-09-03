#pragma once
//
// Isochronic tone generation.
//
// A "10 Hz stimulus" is not a 10 Hz sound -- 10 Hz is far below hearing. It is
// an audible carrier switched on and off ten times a second, which is what
// makes the rate physically deliverable through a speaker.
//
// Pure DSP: fills a buffer, owns no device. The audio device lives in
// audio_engine.hpp and calls render() from its callback.

#include <cstdint>
#include <vector>

#include "elanora/collector/session.hpp"
#include "elanora/types.hpp"

namespace elanora::collector {

class ToneGenerator {
public:
    explicit ToneGenerator(int sample_rate = 48000);

    // Sets up the generator for one round. `hz` is used for Stim rounds in
    // sweep mode; the design supplies the layers for a stacked round.
    void configure(const StimulusDesign& design, Condition cond, double hz,
                   double jitter_mean_hz, uint64_t seed);

    // The gate is opened only during the stimulus phase. Safe to call from any
    // thread: the render callback reads it as a plain bool each block.
    void set_gate(bool open) { gate_ = open; }
    bool gate() const { return gate_; }

    // Fills `frames` interleaved stereo samples. Never allocates -- this runs
    // on the audio thread, where an allocation is a dropout.
    void render(float* out, int frames);

    int sample_rate() const { return sr_; }
    void set_sample_rate(int sr);

    // RMS of a rendered block, exposed so the conditions can be checked for
    // equal loudness in a test rather than by ear.
    static double rms(const float* buf, int frames);

private:
    double next_jitter_gap();

    int    sr_;
    Condition cond_ = Condition::Stim;
    StimMode  mode_ = StimMode::Single;
    double carrier_hz_ = 440.0;
    double duty_ = 0.5;
    double amplitude_ = 0.5;

    // Per-layer rate and amplitude, flattened from the design so render()
    // never touches a std::vector that could reallocate under it.
    static constexpr int kMaxLayers = 6;
    double rate_[kMaxLayers]{};
    double amp_[kMaxLayers]{};
    int    n_layers_ = 0;

    // Phase accumulators, advanced by a fixed increment per sample.
    //
    // Never derived from an absolute frame counter: at 48 kHz a float frame
    // index loses precision within minutes, and the recomputed phase jumps,
    // which is audible as a click in the middle of a stimulus window.
    double carrier_phase_ = 0.0;
    double gate_phase_[kMaxLayers]{};

    // Jitter control state: the next gap is drawn rather than fixed.
    uint64_t rng_ = 1;
    double   jitter_mean_hz_ = 10.0;
    double   jitter_next_ = 0.0;     // seconds until the next pulse starts
    double   jitter_t_ = 0.0;
    double   jitter_width_ = 0.05;
    bool     jitter_on_ = false;

    // Amplitude follower, so the gate never steps discontinuously. A hard edge
    // is a click, and a click is itself a startling stimulus -- it would sit in
    // the recording as an evoked response that has nothing to do with the rate
    // under test.
    double env_ = 0.0;
    double ramp_coeff_ = 0.0;

    bool gate_ = false;
};

}  // namespace elanora::collector
