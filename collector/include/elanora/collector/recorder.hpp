#pragma once
//
// Writes a trial to disk.
//
// The rule the whole project rests on: RAW DATA IS ALWAYS SAVED. Features can
// be recomputed from raw whenever the analysis changes; raw cannot be
// recovered from features, and re-running a subject is not an option.
//
// Files are written per round, never buffered to the end of the trial. A crash
// at round 19 must not cost the eighteen good rounds before it.

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "elanora/collector/session.hpp"
#include "elanora/lsl/stream_recorder.hpp"
#include "elanora/types.hpp"

namespace elanora::collector {

// Timestamped phase boundary, written alongside the samples so a period can be
// sliced out of the raw file afterwards without trusting a clock.
struct Marker {
    double      ts = 0.0;
    std::string event;
};

class TrialRecorder {
public:
    // Creates data/datasets/raw/<session_id>/ and appends a row to
    // sessions.csv. Returns false with a reason if the tree is not writable --
    // better to refuse at the start than to discover it after 40 minutes.
    // `quality_override` records that the operator started with an electrode
    // reading Bad. The trial is still recorded; the analysis needs to know.
    bool begin_trial(const std::filesystem::path& root,
                     const std::string& subject, int trial_number,
                     const StimulusDesign& design, const Durations& d,
                     uint64_t seed, int round_count, std::string& err,
                     bool quality_override = false);

    // One round's raw streams plus its markers and metadata.
    bool write_round(int round_index, const PlannedRound& round,
                     const std::vector<lsl::Sample>& eeg,
                     const std::vector<lsl::Sample>& ppg,
                     const std::vector<lsl::Sample>& imu,
                     const std::vector<Marker>& markers,
                     const lsl::ChannelMap& channels,
                     std::string& err);

    // Appended after the subject answers, keyed to the same trial_id.
    bool write_survey(int round_index, const Survey& s, std::string& err);

    const std::string& session_id() const { return session_id_; }
    const std::filesystem::path& session_dir() const { return session_dir_; }
    int rounds_written() const { return rounds_written_; }
    bool active() const { return active_; }
    void end_trial() { active_ = false; }

private:
    std::string trial_row_id(int round_index) const;

    std::filesystem::path root_;
    std::filesystem::path session_dir_;
    std::string session_id_;
    std::string subject_;
    int  rounds_written_ = 0;
    bool active_ = false;
};

}  // namespace elanora::collector
