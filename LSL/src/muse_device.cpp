#include "elanora/lsl/muse_device.hpp"

#include <algorithm>

namespace elanora::lsl {

namespace {

bool looks_like_mac(const std::string& s) {
    // BrainFlow wants a MAC in mac_address and a device name in serial_number.
    // Colons are the only reliable discriminator between "00:11:22:33:44:55"
    // and "MuseS-1234".
    return s.find(':') != std::string::npos;
}

bool is_muse_board(int board_id) {
    switch (static_cast<BoardIds>(board_id)) {
        case BoardIds::MUSE_2_BOARD:
        case BoardIds::MUSE_S_BOARD:
        case BoardIds::MUSE_2_BLED_BOARD:
        case BoardIds::MUSE_S_BLED_BOARD:
        case BoardIds::MUSE_2016_BOARD:
        case BoardIds::MUSE_2016_BLED_BOARD:
            return true;
        default:
            return false;
    }
}

// Every BoardShim static throws when the board does not expose the stream.
// Absence is normal, not exceptional, so it is swallowed into an empty vector.
std::vector<int> try_channels(std::vector<int> (*fn)(int, int), int board_id, int preset) {
    try {
        return fn(board_id, preset);
    } catch (const BrainFlowException&) {
        return {};
    }
}

int try_timestamp(int board_id, int preset) {
    try {
        return BoardShim::get_timestamp_channel(board_id, preset);
    } catch (const BrainFlowException&) {
        return -1;
    }
}

int try_rate(int board_id, int preset) {
    try {
        return BoardShim::get_sampling_rate(board_id, preset);
    } catch (const BrainFlowException&) {
        return 0;
    }
}

}  // namespace

MuseDevice::MuseDevice(int board_id) : board_id_(board_id) {}

MuseDevice::~MuseDevice() { disconnect(); }

bool MuseDevice::connect(const std::string& serial_or_mac, std::string& err) {
    err.clear();
    if (connected_) return true;

    try {
        BrainFlowInputParams params;
        if (!serial_or_mac.empty()) {
            if (looks_like_mac(serial_or_mac)) {
                params.mac_address = serial_or_mac;
            } else {
                params.serial_number = serial_or_mac;
            }
        }
        // Without a timeout BrainFlow can block indefinitely hunting for a
        // headset that is not switched on, which looks like a frozen UI.
        params.timeout = 15;

        board_ = std::make_unique<BoardShim>(board_id_, params);
        board_->prepare_session();

        // PPG is off until asked for. p50 also enables the fifth EEG channel;
        // harmless here since channels are resolved from BrainFlow afterwards.
        if (is_muse_board(board_id_)) {
            try {
                board_->config_board("p50");
            } catch (const BrainFlowException& e) {
                // A headset without PPG is still usable for EEG, so this is a
                // degradation rather than a failure.
                err = std::string("PPG not enabled: ") + e.what();
            }
        }

        board_->start_stream();
        connected_ = true;
        resolve_channels();
        return true;
    } catch (const BrainFlowException& e) {
        err = e.what();
        // Roll back so a failed connect cannot leave a half-prepared session
        // that makes the next attempt fail for a different reason.
        if (board_) {
            try { board_->release_session(); } catch (const BrainFlowException&) {}
            board_.reset();
        }
        connected_ = false;
        return false;
    }
}

void MuseDevice::disconnect() {
    if (!board_) {
        connected_ = false;
        return;
    }
    try {
        if (connected_) board_->stop_stream();
    } catch (const BrainFlowException&) {
    }
    try {
        board_->release_session();
    } catch (const BrainFlowException&) {
    }
    board_.reset();
    connected_ = false;
}

BrainFlowArray<double, 2> MuseDevice::pull(BrainFlowPresets preset) {
    if (!connected_ || !board_) return {};
    try {
        return board_->get_board_data(static_cast<int>(preset));
    } catch (const BrainFlowException&) {
        return {};
    }
}

bool MuseDevice::configure(const std::string& command, std::string& err) {
    err.clear();
    if (!connected_ || !board_) {
        err = "not connected";
        return false;
    }
    try {
        board_->config_board(command);
        return true;
    } catch (const BrainFlowException& e) {
        err = e.what();
        return false;
    }
}

void MuseDevice::resolve_channels() {
    const int def = static_cast<int>(BrainFlowPresets::DEFAULT_PRESET);
    const int aux = static_cast<int>(BrainFlowPresets::AUXILIARY_PRESET);
    const int anc = static_cast<int>(BrainFlowPresets::ANCILLARY_PRESET);

    channels_ = ChannelMap{};
    channels_.eeg   = try_channels(&BoardShim::get_eeg_channels,   board_id_, def);
    channels_.ppg   = try_channels(&BoardShim::get_ppg_channels,   board_id_, anc);
    channels_.accel = try_channels(&BoardShim::get_accel_channels, board_id_, aux);
    channels_.gyro  = try_channels(&BoardShim::get_gyro_channels,  board_id_, aux);

    channels_.ts_eeg = try_timestamp(board_id_, def);
    channels_.ts_ppg = try_timestamp(board_id_, anc);
    channels_.ts_imu = try_timestamp(board_id_, aux);

    channels_.sr_eeg = try_rate(board_id_, def);
    channels_.sr_ppg = try_rate(board_id_, anc);
    channels_.sr_imu = try_rate(board_id_, aux);
}

}  // namespace elanora::lsl
