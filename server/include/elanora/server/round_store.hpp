#pragma once
//
// Receives one recorded round from the phone and writes it into the dataset.
//
// The phone is the only recorder this project currently has, so this is the
// path every future observation travels. It validates before it writes: a
// malformed upload must not be able to produce a file that later makes
// elanora_data read wrong numbers, which is a far worse failure than a
// rejected upload because nothing announces it.

#include <filesystem>
#include <string>

namespace elanora::server {

struct RoundUpload {
    std::string session_id;
    std::string trial_id;
    std::string subject_id;
    std::string condition;        // stim | control_jitter | control_tone
    int         round_index = 0;
    double      frequency_hz = 0.0;
    double      jitter_mean_hz = 0.0;

    // Each is a complete CSV including its header row, exactly as it will be
    // written. The phone formats them, because it is the only place that knows
    // the reconstructed timestamps.
    std::string eeg_csv;
    std::string ppg_csv;
    std::string imu_csv;
    std::string markers_csv;

    // Set when the phone detected a suspended tab or a BLE dropout during the
    // round. Kept rather than discarded -- a partial round is evidence about
    // the session, and silently dropping it would hide a systematic problem.
    bool suspect = false;
};

// Reads the JSON envelope the phone posts. Returns false with a reason rather
// than throwing, because the reason is shown to the operator on the phone.
bool parse_round(const std::string& body, RoundUpload& out, std::string& err);

// Writes raw/<session_id>/<trial_id>_{eeg,ppg,imu,markers}.csv and appends the
// trials.csv row. Never overwrites: uploads are additive.
bool store_round(const std::filesystem::path& root, const RoundUpload& r,
                 std::string& err);

}  // namespace elanora::server
