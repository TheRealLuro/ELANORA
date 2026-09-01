#pragma once
//
// Collector tab UI.
//
// Separate target from elanora_collector on purpose: the logic library stays
// free of ImGui so the experiment's rules can be tested without a window, and
// this is the only piece that knows about drawing.

#include <array>
#include <cstdint>

#include "elanora/collector/session.hpp"
#include "elanora/lsl/signal_quality.hpp"

namespace elanora::collector {

// Everything the tab owns between frames. Held by the app so switching tabs
// never loses a running trial.
struct CollectorState {
    StimulusDesign design;
    TrialRunner    runner;
    Survey         survey;
    Durations      durations;

    std::string subject = "P01";
    std::string trial_id = "S01";
    uint64_t    seed = 84120;

    double  freq_lo = 0.5;
    double  freq_hi = 45.0;
    int     freq_count = 16;
    int     n_jitter = 2;
    int     n_tone = 2;

    // Compresses the clock so a 40-minute trial can be walked through in a
    // couple of minutes while checking the flow. Never used for real data.
    double  speed = 1.0;

    bool    survey_open = false;

    CollectorState();
    std::vector<PlannedRound> preview_schedule() const;
};

// Draws the whole tab and advances the trial clock. `qual` is the live
// electrode state, shown during a run so a headset problem is caught in the
// round it happens rather than at analysis.
void draw_collector(CollectorState& st,
                    const std::array<lsl::ChannelQuality, kSensorCount>& qual,
                    float dt);

}  // namespace elanora::collector
