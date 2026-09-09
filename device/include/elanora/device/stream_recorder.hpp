#pragma once
//
// Buffers the three Muse 2 streams and serves time windows out of them.
//
// The collector cannot ask the device for "the 30 seconds that just happened"
// -- BrainFlow hands out whatever has arrived since the last call and then
// forgets it. So a poll thread drains all three presets continuously into
// per-preset buffers, and window() slices out a period after the fact.
//
// Threading: poll() runs on a background thread and writes; window() and the
// size accessors run on the UI thread and read. All are mutex guarded.

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "elanora/device/muse_device.hpp"

namespace elanora::device {

// One instant across every channel of one preset. `values` holds every row
// BrainFlow returned, including the timestamp row, so nothing is discarded at
// capture time and new features can be derived later without re-recording.
struct Sample {
    double              ts = 0.0;
    std::vector<double> values;
};

class StreamRecorder {
public:
    // Buffer depth in seconds. A trial is 90 s plus 30 s rest; 240 s leaves
    // room for a late window() call without losing the start of the trial.
    static constexpr double kBufferSeconds = 240.0;

    void start(MuseDevice& device);
    void stop();
    bool running() const;

    // Drains all three presets once. Call repeatedly, ~20 Hz is plenty.
    void poll();

    // Samples with t0 <= ts < t1, in timestamp order. Returns empty rather
    // than throwing when the range predates the buffer or nothing matches.
    std::vector<Sample> window(BrainFlowPresets preset, double t0, double t1) const;

    std::size_t size(BrainFlowPresets preset) const;

    // Timestamp of the newest buffered sample, or 0 if the stream is empty.
    // Used by the UI to show stream liveness.
    double latest_ts(BrainFlowPresets preset) const;

    // Discards everything buffered. Called between sessions.
    void clear();

private:
    struct Stream {
        std::deque<Sample> samples;
        int                ts_channel = -1;
        int                rate       = 0;
    };

    Stream&       stream_for(BrainFlowPresets preset);
    const Stream& stream_for(BrainFlowPresets preset) const;

    void drain(BrainFlowPresets preset);
    void trim(Stream& s) const;

    mutable std::mutex mutex_;
    MuseDevice*        device_  = nullptr;
    bool               running_ = false;

    Stream eeg_;  // DEFAULT_PRESET
    Stream imu_;  // AUXILIARY_PRESET
    Stream ppg_;  // ANCILLARY_PRESET
};

}  // namespace elanora::device
