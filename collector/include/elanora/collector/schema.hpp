#pragma once
//
// The dataset's column layout, in one place.
//
// Two writers produce these files: TrialRecorder on the desktop, and the web
// collector's server from phone uploads. When the columns lived only as string
// literals inside recorder.cpp, adding the second writer meant a second copy of
// every header, and two copies of a schema drift until elanora_data reads one
// of them wrong -- silently, because a CSV with the wrong columns still parses.
//
// Changing a column here changes it for both writers, and the schema test
// fails if the two ever disagree.

#include <string>
#include <vector>

namespace elanora::collector {

inline const std::vector<std::string> kSessionsHeader = {
    "session_id", "subject_id", "trial_number", "date", "stim_mode",
    "carrier_hz", "duty_cycle", "baseline_s", "stimulus_s", "post_s", "rest_s",
    "order_seed", "round_count", "quality_override"};

inline const std::vector<std::string> kTrialsHeader = {
    "trial_id", "session_id", "subject_id", "round_index", "condition",
    "frequency_hz", "jitter_mean_hz", "started_at", "n_eeg", "n_ppg", "n_imu"};

inline const std::vector<std::string> kSurveysHeader = {
    "trial_id", "session_id", "subject_id", "round_index", "relaxation", "alertness",
    "pleasantness", "discomfort", "breathing_perceived", "breaths_self_count",
    "heard_rhythm", "artifact_jaw", "artifact_move", "artifact_eyes",
    "artifact_swallow", "artifact_noise", "note"};

// The per-round raw streams. The EEG column names are the electrode names in
// sensor order S1..S4, which is the order every downstream reader assumes.
inline const std::vector<std::string> kEegHeader = {
    "timestamp", "TP9", "AF7", "AF8", "TP10"};
inline const std::vector<std::string> kPpgHeader = {
    "timestamp", "ppg_red", "ppg_ir", "ppg_ambient"};
inline const std::vector<std::string> kImuHeader = {
    "timestamp", "ax", "ay", "az", "gx", "gy", "gz"};
inline const std::vector<std::string> kMarkersHeader = {"timestamp", "event"};

}  // namespace elanora::collector
