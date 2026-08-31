// Muse 2 channel map probe.
//
// Asks BrainFlow what it reports for MUSE_2_BOARD without connecting to any
// hardware. Two jobs: prove the BrainFlow integration links and runs, and give
// a standing check on the constants the rest of ELANORA is built around --
// 256 Hz EEG on four channels, ~52 Hz IMU, ~64 Hz two-channel PPG.
//
// If this output ever stops matching the sensor mapping in types.hpp, every
// dataset written afterwards would be mislabelled.

#include <cstdio>
#include <string>
#include <vector>

#include "board_shim.h"
#include "brainflow_constants.h"

namespace {

void print_channels(const char* label, const std::vector<int>& ch) {
    std::printf("    %-22s (%zu) [", label, ch.size());
    for (std::size_t i = 0; i < ch.size(); ++i) {
        std::printf("%s%d", i ? ", " : "", ch[i]);
    }
    std::printf("]\n");
}

void probe_preset(const char* name, int board_id, BrainFlowPresets preset) {
    const int p = static_cast<int>(preset);
    std::printf("  %s\n", name);
    try {
        std::printf("    %-22s %d Hz\n", "sampling rate",
                    BoardShim::get_sampling_rate(board_id, p));
    } catch (const BrainFlowException& e) {
        std::printf("    sampling rate          unavailable (%s)\n", e.what());
    }

    struct Q { const char* label; std::vector<int> (*fn)(int, int); };
    const Q queries[] = {
        {"eeg channels",   &BoardShim::get_eeg_channels},
        {"ppg channels",   &BoardShim::get_ppg_channels},
        {"accel channels", &BoardShim::get_accel_channels},
        {"gyro channels",  &BoardShim::get_gyro_channels},
    };
    for (const auto& q : queries) {
        try {
            print_channels(q.label, q.fn(board_id, p));
        } catch (const BrainFlowException&) {
            // Not every preset carries every modality; absence is normal.
        }
    }
    try {
        std::printf("    %-22s %d\n", "timestamp channel",
                    BoardShim::get_timestamp_channel(board_id, p));
    } catch (const BrainFlowException&) {
    }
    std::printf("\n");
}

}  // namespace

int main() {
    BoardShim::disable_board_logger();

    const int board_id = static_cast<int>(BoardIds::MUSE_2_BOARD);
    std::printf("BrainFlow version : %s\n", BoardShim::get_version().c_str());
    std::printf("MUSE_2_BOARD id   : %d\n\n", board_id);

    try {
        const auto names = BoardShim::get_eeg_names(board_id);
        std::printf("  EEG electrode names: ");
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::printf("%s%s", i ? ", " : "", names[i].c_str());
        }
        std::printf("\n\n");
    } catch (const BrainFlowException& e) {
        std::printf("  EEG names unavailable: %s\n\n", e.what());
    }

    probe_preset("DEFAULT_PRESET   (EEG)",        board_id, BrainFlowPresets::DEFAULT_PRESET);
    probe_preset("AUXILIARY_PRESET (accel/gyro)", board_id, BrainFlowPresets::AUXILIARY_PRESET);
    probe_preset("ANCILLARY_PRESET (PPG)",        board_id, BrainFlowPresets::ANCILLARY_PRESET);

    return 0;
}
