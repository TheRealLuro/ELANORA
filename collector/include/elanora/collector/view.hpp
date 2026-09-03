#pragma once
//
// Collector tab UI.
//
// Separate target from elanora_collector on purpose: the logic library stays
// free of ImGui so the experiment's rules can be tested without a window, and
// this is the only piece that knows about drawing.

#include <array>
#include <cstdint>

#include "elanora/collector/audio_engine.hpp"
#include "elanora/collector/recorder.hpp"
#include "elanora/collector/session.hpp"
#include "elanora/lsl/signal_quality.hpp"
#include "elanora/lsl/stream_recorder.hpp"

namespace elanora::collector {

// Everything the tab owns between frames. Held by the app so switching tabs
// never loses a running trial.
struct CollectorState {
    StimulusDesign design;
    TrialRunner    runner;
    Survey         survey;
    Durations      durations;

    // The only field that changes between people. Kept as a char buffer so
    // ImGui can edit it directly.
    char     subject[32] = "P01";
    int      trial_number = 1;
    uint64_t seed = 84120;

    // Stimulus design is hidden by default. The protocol is fixed, so the
    // common case is: type a name, press start.
    bool advanced_open = false;

    // Editable, but seeded from the protocol constants rather than repeating
    // the literals here -- the spacing is an experimental constraint, and a
    // second copy of it is a second place for it to drift.
    double  freq_lo    = kProtocolFreqLo;
    double  freq_hi    = kProtocolFreqHi;
    int     freq_count = kProtocolFreqCount;
    int     n_jitter   = kProtocolJitter;
    int     n_tone     = kProtocolTone;

    // Compresses the clock so a 40-minute trial can be walked through in a
    // couple of minutes while checking the flow. Never used for real data.
    double  speed = 1.0;

    bool    survey_open = false;

    // Set when the operator chose to start with an electrode reading Bad. The
    // trial still runs; the fact is recorded so the analysis can see it.
    bool override_quality = false;
    bool started_with_bad_electrodes = false;

    AudioEngine   audio;
    TrialRecorder recorder;
    std::string   io_status;      // shown in the UI; empty when all is well

    // Markers for the round in flight, stamped from the board clock so a phase
    // can be sliced out of the raw file without trusting a wall clock.
    std::vector<Marker> markers;
    Phase last_phase = Phase::Baseline;
    int   last_round = -1;
    double round_start_ts = 0.0;

    CollectorState();

    // A fresh order for every trial. Reusing one seed across subjects would
    // give everybody the identical round order, which confounds frequency with
    // position in the session for the entire study rather than just one run.
    void reseed();
    std::vector<PlannedRound> preview_schedule() const;
};

// Draws the whole tab and advances the trial clock. `qual` is the live
// electrode state, shown during a run so a headset problem is caught in the
// round it happens rather than at analysis.
// `stream` and `channels` supply the raw samples written to disk; pass nullptr
// when no device is connected and the trial runs as a silent rehearsal.
void draw_collector(CollectorState& st,
                    const std::array<lsl::ChannelQuality, kSensorCount>& qual,
                    lsl::StreamRecorder* stream,
                    const lsl::ChannelMap* channels,
                    float dt);

}  // namespace elanora::collector
