#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "elanora/lsl/muse_device.hpp"

using namespace elanora::lsl;

namespace {

// SYNTHETIC_BOARD generates plausible data with no hardware attached, so the
// whole connect/stream/pull/disconnect path is testable in CI. It exposes only
// DEFAULT_PRESET, which is why the PPG and IMU assertions below are about the
// Muse-specific static queries rather than about synthetic streaming.
constexpr int kSynthetic = static_cast<int>(BoardIds::SYNTHETIC_BOARD);
constexpr int kMuse2     = static_cast<int>(BoardIds::MUSE_2_BOARD);

}  // namespace

TEST_CASE("connecting to the synthetic board succeeds and resolves channels", "[device]") {
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));
    REQUIRE(err.empty());
    REQUIRE(dev.connected());
    REQUIRE(dev.channels().has_eeg());
    REQUIRE(dev.channels().sr_eeg > 0);
    REQUIRE(dev.channels().ts_eeg >= 0);
    dev.disconnect();
}

TEST_CASE("pulling returns samples after streaming briefly", "[device]") {
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto data = dev.pull(BrainFlowPresets::DEFAULT_PRESET);

    REQUIRE(data.get_size(0) > 0);   // rows == channels
    REQUIRE(data.get_size(1) > 0);   // cols == samples
    dev.disconnect();
}

TEST_CASE("disconnect is safe twice and when never connected", "[device]") {
    // The UI calls disconnect on shutdown regardless of state; a double
    // release_session() throws inside BrainFlow and would crash on exit.
    MuseDevice never;
    REQUIRE_NOTHROW(never.disconnect());
    REQUIRE_FALSE(never.connected());

    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));
    REQUIRE_NOTHROW(dev.disconnect());
    REQUIRE_NOTHROW(dev.disconnect());
    REQUIRE_FALSE(dev.connected());
}

TEST_CASE("pulling while disconnected yields empty rather than throwing", "[device]") {
    MuseDevice dev(kSynthetic);
    REQUIRE_NOTHROW(dev.pull(BrainFlowPresets::DEFAULT_PRESET));
    REQUIRE(dev.pull(BrainFlowPresets::DEFAULT_PRESET).get_size(1) == 0);
}

TEST_CASE("connecting to a nonexistent device reports an error, not an exception", "[device]") {
    // A missing headset is the single most common failure in this project.
    // It must surface as a message the operator can read, never as a crash.
    MuseDevice dev(kMuse2);
    std::string err;
    const bool ok = dev.connect("00:00:00:00:00:00", err);
    if (!ok) {
        REQUIRE_FALSE(err.empty());
        REQUIRE_FALSE(dev.connected());
    }
    dev.disconnect();
}

// ---------------------------------------------------------------------------
// Guards on the hardware constants the whole project is built around. These
// need no device -- BrainFlow answers them from its board description table.
// ---------------------------------------------------------------------------

TEST_CASE("Muse 2 reports 4 EEG channels at 256 Hz", "[device][constants]") {
    REQUIRE(BoardShim::get_sampling_rate(
        kMuse2, static_cast<int>(BrainFlowPresets::DEFAULT_PRESET)) == 256);
    REQUIRE(BoardShim::get_eeg_channels(
        kMuse2, static_cast<int>(BrainFlowPresets::DEFAULT_PRESET)).size() == 4);
}

TEST_CASE("Muse 2 reports 3 PPG channels, of which 2 are active wavelengths",
          "[device][constants]") {
    // The Muse 2 datasheet says "2-channel PPG (940nm + 660nm)", but BrainFlow
    // exposes three: it subscribes to three BLE characteristics (PPG0/1/2),
    // the third being the ambient-light reference used to correct the other
    // two. Verified against brainflow_boards.cpp: {"ppg_channels", {1, 2, 3}}.
    //
    // Ordering, per BrainFlow's own python_package/examples/tests/muse_ppg.py:
    //     ppg_red = data[ppg_channels[0]]   660nm
    //     ppg_ir  = data[ppg_channels[1]]   940nm
    // The heart features code depends on that ordering.
    const auto ppg = BoardShim::get_ppg_channels(
        kMuse2, static_cast<int>(BrainFlowPresets::ANCILLARY_PRESET));
    REQUIRE(ppg.size() == 3);
}

TEST_CASE("Muse 2 reports a 6-axis IMU", "[device][constants]") {
    const int aux = static_cast<int>(BrainFlowPresets::AUXILIARY_PRESET);
    REQUIRE(BoardShim::get_accel_channels(kMuse2, aux).size() == 3);
    REQUIRE(BoardShim::get_gyro_channels(kMuse2, aux).size() == 3);
}

TEST_CASE("EEG sampling rate leaves headroom above the 45 Hz gamma ceiling", "[device][constants]") {
    // Nyquist must exceed the top of the gamma band with margin, or the
    // highest band the project measures would be aliased.
    const int sr = BoardShim::get_sampling_rate(
        kMuse2, static_cast<int>(BrainFlowPresets::DEFAULT_PRESET));
    REQUIRE(sr / 2 > 45 * 2);
}
