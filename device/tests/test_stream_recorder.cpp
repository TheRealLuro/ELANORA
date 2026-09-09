#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

#include "elanora/device/stream_recorder.hpp"

using namespace elanora::device;

namespace {

constexpr int kSynthetic = static_cast<int>(BoardIds::SYNTHETIC_BOARD);
constexpr auto kDefault  = BrainFlowPresets::DEFAULT_PRESET;

// Polls for `seconds`, the way the UI thread does, so the buffer fills from
// many small pulls rather than one big one.
void pump(StreamRecorder& rec, double seconds) {
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (std::chrono::steady_clock::now() < until) {
        rec.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    rec.poll();
}

}  // namespace

TEST_CASE("a one second window holds about one second of samples", "[recorder]") {
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));

    StreamRecorder rec;
    rec.start(dev);
    pump(rec, 2.5);

    const double newest = rec.latest_ts(kDefault);
    REQUIRE(newest > 0.0);

    const auto w = rec.window(kDefault, newest - 1.0, newest);
    const int rate = dev.channels().sr_eeg;
    REQUIRE(rate > 0);

    // Generous margin: BLE and synthetic generators both deliver in bursts,
    // so an exact count would be flaky for reasons that are not bugs.
    REQUIRE(static_cast<int>(w.size()) > rate * 3 / 4);
    REQUIRE(static_cast<int>(w.size()) < rate * 5 / 4);

    rec.stop();
    dev.disconnect();
}

TEST_CASE("buffered samples are in timestamp order", "[recorder]") {
    // window() binary-searches on timestamp, which silently returns wrong
    // ranges if the buffer is ever out of order.
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));

    StreamRecorder rec;
    rec.start(dev);
    pump(rec, 1.5);

    const auto w = rec.window(kDefault, 0.0, 1e18);
    REQUIRE(w.size() > 1);
    REQUIRE(std::is_sorted(w.begin(), w.end(),
                           [](const Sample& a, const Sample& b) { return a.ts < b.ts; }));

    rec.stop();
    dev.disconnect();
}

TEST_CASE("a window before the recording started is empty, not an error", "[recorder]") {
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));

    StreamRecorder rec;
    rec.start(dev);
    pump(rec, 0.8);

    // Unix epoch 0 to 1 -- decades before anything in the buffer.
    std::vector<Sample> w;
    REQUIRE_NOTHROW(w = rec.window(kDefault, 0.0, 1.0));
    REQUIRE(w.empty());

    rec.stop();
    dev.disconnect();
}

TEST_CASE("each sample carries every channel row", "[recorder]") {
    // Raw data is never discarded at capture time, so new features can be
    // derived later without repeating the experiment.
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));

    StreamRecorder rec;
    rec.start(dev);
    pump(rec, 0.8);

    const auto w = rec.window(kDefault, 0.0, 1e18);
    REQUIRE_FALSE(w.empty());
    const std::size_t n_rows = w.front().values.size();
    REQUIRE(n_rows > static_cast<std::size_t>(dev.channels().eeg.size()));
    for (const auto& s : w) REQUIRE(s.values.size() == n_rows);

    rec.stop();
    dev.disconnect();
}

TEST_CASE("polling while stopped is harmless", "[recorder]") {
    StreamRecorder rec;
    REQUIRE_FALSE(rec.running());
    REQUIRE_NOTHROW(rec.poll());
    REQUIRE(rec.size(kDefault) == 0);
    REQUIRE(rec.latest_ts(kDefault) == 0.0);
}

TEST_CASE("clear empties every stream", "[recorder]") {
    MuseDevice dev(kSynthetic);
    std::string err;
    REQUIRE(dev.connect("", err));

    StreamRecorder rec;
    rec.start(dev);
    pump(rec, 0.8);
    REQUIRE(rec.size(kDefault) > 0);

    rec.clear();
    REQUIRE(rec.size(kDefault) == 0);

    rec.stop();
    dev.disconnect();
}
