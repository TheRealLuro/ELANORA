#pragma once
//
// Thin, exception-free wrapper over BrainFlow's BoardShim for the Muse 2.
//
// Two jobs beyond forwarding calls:
//
//   1. Channel indices are resolved once at connect time and cached in a
//      ChannelMap. Nothing downstream hardcodes an index -- BrainFlow's layout
//      differs per board and per preset, and a wrong index silently produces
//      plausible-looking garbage rather than an error.
//
//   2. Every BrainFlow call is wrapped so no BrainFlowException escapes into
//      the UI thread. Failures come back as a bool plus a message.
//
// The board id is a constructor parameter so tests can drive SYNTHETIC_BOARD
// with no hardware attached.

#include <memory>
#include <string>
#include <vector>

#include "board_shim.h"
#include "brainflow_constants.h"

namespace elanora::device {

// Channel indices and sampling rates for the three Muse 2 presets, resolved
// from BrainFlow at connect time. An index of -1 means the board does not
// expose that stream.
struct ChannelMap {
    std::vector<int> eeg;    // DEFAULT_PRESET   -- TP9, AF7, AF8, TP10
    std::vector<int> ppg;    // ANCILLARY_PRESET -- [0]=red 660nm, [1]=IR 940nm
    std::vector<int> accel;  // AUXILIARY_PRESET
    std::vector<int> gyro;   // AUXILIARY_PRESET

    int ts_eeg = -1;
    int ts_ppg = -1;
    int ts_imu = -1;

    int sr_eeg = 0;  // expected 256 Hz on Muse 2
    int sr_ppg = 0;  // expected 64 Hz
    int sr_imu = 0;  // expected 52 Hz

    bool has_eeg() const { return !eeg.empty(); }
    bool has_ppg() const { return !ppg.empty(); }
    bool has_imu() const { return !accel.empty() || !gyro.empty(); }
};

// How the headset is reached.
//
// The Muse 2 has no USB data interface -- the micro-USB socket charges the
// headset and nothing more, so EEG can never arrive over the charging cable.
// "Connecting over USB" therefore means one of exactly two things, and only
// the second needs different code:
//
//   NativeBle    The PC's own Bluetooth radio, driven through SimpleBLE.
//                A plain USB Bluetooth adapter shows up here -- Windows owns
//                it, BrainFlow just sees a working BLE stack. No board change.
//
//   BledDongle   A Silicon Labs BLED112 USB dongle, which is a BLE radio that
//                presents itself as a serial port. BrainFlow talks to it
//                directly over that COM port, bypassing the Windows Bluetooth
//                stack entirely, so it works on a machine with no Bluetooth
//                hardware at all. This is a different board id.
enum class Transport {
    NativeBle,
    BledDongle,
};

const char* transport_name(Transport t);

// The BrainFlow board id backing a transport.
int board_for(Transport t);

// Serial ports the OS currently reports, e.g. {"COM1", "COM4"}. Used to offer
// a choice rather than making the operator guess which port the dongle took.
// Returns an empty vector on platforms where enumeration is unavailable.
std::vector<std::string> list_serial_ports();

struct ConnectRequest {
    Transport   transport = Transport::NativeBle;
    // Optional. Empty means "the first Muse BrainFlow finds".
    std::string serial_or_mac;
    // Required for BledDongle, ignored otherwise. e.g. "COM4".
    std::string serial_port;
};

class MuseDevice {
public:
    explicit MuseDevice(int board_id = static_cast<int>(BoardIds::MUSE_2_BOARD));
    ~MuseDevice();

    MuseDevice(const MuseDevice&)            = delete;
    MuseDevice& operator=(const MuseDevice&) = delete;

    // Prepares the session, enables PPG, starts streaming, and resolves the
    // channel map. `serial_or_mac` may be empty to let BrainFlow discover the
    // first matching device. Returns false and fills `err` on any failure.
    bool connect(const std::string& serial_or_mac, std::string& err);

    // As above, but selects the transport too.
    //
    // The board id switches to match `req.transport`, but only when this
    // device was constructed as a Muse in the first place. A MuseDevice built
    // on SYNTHETIC_BOARD stays synthetic whatever transport is asked for --
    // otherwise every test would start hunting for real Bluetooth hardware.
    bool connect(const ConnectRequest& req, std::string& err);

    // Safe to call when not connected, and safe to call twice.
    void disconnect();

    bool connected() const { return connected_; }
    const ChannelMap& channels() const { return channels_; }
    int board_id() const { return board_id_; }

    // Drains buffered samples for one preset. Returns an empty array rather
    // than throwing when not connected or when nothing has arrived yet.
    BrainFlowArray<double, 2> pull(BrainFlowPresets preset);

    // Passes a raw command to the board (e.g. "p50" to enable PPG).
    bool configure(const std::string& command, std::string& err);

private:
    // Fills channels_ from BoardShim statics. Never throws.
    void resolve_channels();

    int         board_id_;
    bool        connected_ = false;
    ChannelMap  channels_;
    std::unique_ptr<BoardShim> board_;
};

}  // namespace elanora::device
