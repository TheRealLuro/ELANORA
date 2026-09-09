#include "elanora/device/muse_device.hpp"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace elanora::device {

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

// BrainFlow's what() is the message with the numeric exit code glued onto the
// end -- "failed to prepare session3" -- which tells an operator nothing about
// what to do. These are the failures actually reachable while connecting a
// Muse, rewritten as the next action to take.
std::string explain(const BrainFlowException& e, Transport t, const std::string& port) {
    // The no-radio case surfaces here rather than as GENERAL_ERROR: SimpleBLE
    // logs "No BLE adapters found" and BrainFlow turns that into an
    // unable-to-open-port. Verified against this machine, which has no
    // Bluetooth hardware at all.
    static const char* kNoAdapter =
        "no Bluetooth adapter found -- plug in a USB Bluetooth adapter, or use a "
        "BLED112 dongle and switch to USB";

    switch (static_cast<BrainFlowExitCodes>(e.exit_code)) {
        case BrainFlowExitCodes::UNABLE_TO_OPEN_PORT_ERROR:
        case BrainFlowExitCodes::SET_PORT_ERROR:
            if (t == Transport::BledDongle) {
                return "cannot open " + (port.empty() ? std::string("the serial port") : port) +
                       " -- check the BLED112 dongle is plugged in and nothing else is using "
                       "the port";
            }
            return kNoAdapter;
        case BrainFlowExitCodes::BOARD_NOT_READY_ERROR:
            return t == Transport::BledDongle
                       ? "no headset answered on " + port +
                             " -- switch the Muse on and hold it near the dongle"
                       : "no headset answered -- switch the Muse on and hold it near the PC";
        case BrainFlowExitCodes::GENERAL_ERROR:
            return t == Transport::NativeBle ? kNoAdapter : std::string(e.what());
        case BrainFlowExitCodes::INVALID_ARGUMENTS_ERROR:
            return t == Transport::BledDongle
                       ? "invalid serial port -- pick the COM port the dongle registered as"
                       : std::string(e.what());
        default:
            return e.what();
    }
}

}  // namespace

const char* transport_name(Transport t) {
    return t == Transport::BledDongle ? "USB dongle" : "Bluetooth";
}

int board_for(Transport t) {
    return static_cast<int>(t == Transport::BledDongle ? BoardIds::MUSE_2_BLED_BOARD
                                                       : BoardIds::MUSE_2_BOARD);
}

std::vector<std::string> list_serial_ports() {
    std::vector<std::string> ports;
#if defined(_WIN32)
    // Read the ports the OS has actually registered rather than probing COM1
    // through COM255 by opening each one: opening a port can reset the device
    // behind it, and a BLED112 that has just been reset will not answer.
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return ports;   // no serial hardware at all is normal, not an error
    }
    for (DWORD i = 0;; ++i) {
        char  name[256];
        BYTE  value[256];
        DWORD name_len  = sizeof(name);
        DWORD value_len = sizeof(value);
        DWORD type      = 0;
        const LSTATUS rc = RegEnumValueA(key, i, name, &name_len, nullptr, &type,
                                         value, &value_len);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) break;
        if (type != REG_SZ) continue;
        // value_len counts the NUL for REG_SZ, and the registry does not
        // promise one is present. Bound the length before constructing.
        std::size_t n = value_len;
        while (n > 0 && value[n - 1] == '\0') --n;
        if (n > 0) ports.emplace_back(reinterpret_cast<const char*>(value), n);
    }
    RegCloseKey(key);
    std::sort(ports.begin(), ports.end());
#endif
    return ports;
}

MuseDevice::MuseDevice(int board_id) : board_id_(board_id) {}

MuseDevice::~MuseDevice() { disconnect(); }

bool MuseDevice::connect(const std::string& serial_or_mac, std::string& err) {
    ConnectRequest req;
    req.serial_or_mac = serial_or_mac;
    return connect(req, err);
}

bool MuseDevice::connect(const ConnectRequest& req, std::string& err) {
    err.clear();
    if (connected_) return true;

    // Only a Muse changes board id with the transport. A device constructed on
    // SYNTHETIC_BOARD must stay synthetic or the tests would go looking for
    // real radios.
    if (is_muse_board(board_id_)) board_id_ = board_for(req.transport);

    if (req.transport == Transport::BledDongle && req.serial_port.empty()) {
        err = "a BLED112 dongle needs a serial port, e.g. COM4";
        return false;
    }

    try {
        BrainFlowInputParams params;
        const std::string& serial_or_mac = req.serial_or_mac;
        if (!serial_or_mac.empty()) {
            if (looks_like_mac(serial_or_mac)) {
                params.mac_address = serial_or_mac;
            } else {
                params.serial_number = serial_or_mac;
            }
        }
        // The dongle is reached as a serial device; the native path ignores it.
        params.serial_port = req.serial_port;
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
        err = explain(e, req.transport, req.serial_port);
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

}  // namespace elanora::device
