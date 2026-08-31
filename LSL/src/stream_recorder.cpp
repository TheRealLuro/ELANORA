#include "elanora/lsl/stream_recorder.hpp"

#include <algorithm>

namespace elanora::lsl {

void StreamRecorder::start(MuseDevice& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    device_  = &device;
    running_ = true;

    const ChannelMap& ch = device.channels();
    eeg_ = Stream{};
    imu_ = Stream{};
    ppg_ = Stream{};
    eeg_.ts_channel = ch.ts_eeg;  eeg_.rate = ch.sr_eeg;
    imu_.ts_channel = ch.ts_imu;  imu_.rate = ch.sr_imu;
    ppg_.ts_channel = ch.ts_ppg;  ppg_.rate = ch.sr_ppg;
}

void StreamRecorder::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    device_  = nullptr;
}

bool StreamRecorder::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void StreamRecorder::poll() {
    // Checked without holding the lock across the pulls: get_board_data can
    // block briefly, and holding the mutex through it would stall the UI
    // thread's window() calls for no reason.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || device_ == nullptr) return;
    }
    drain(BrainFlowPresets::DEFAULT_PRESET);
    drain(BrainFlowPresets::AUXILIARY_PRESET);
    drain(BrainFlowPresets::ANCILLARY_PRESET);
}

void StreamRecorder::drain(BrainFlowPresets preset) {
    MuseDevice* dev = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        dev = device_;
    }
    if (dev == nullptr) return;

    const auto data = dev->pull(preset);
    const int rows = data.get_size(0);
    const int cols = data.get_size(1);
    if (rows <= 0 || cols <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    Stream& s = stream_for(preset);

    // A preset whose timestamp row is unknown cannot be windowed, and storing
    // it under a fabricated timestamp would corrupt every later slice.
    if (s.ts_channel < 0 || s.ts_channel >= rows) return;

    for (int col = 0; col < cols; ++col) {
        Sample sample;
        sample.ts = data.at(s.ts_channel, col);
        sample.values.resize(static_cast<std::size_t>(rows));
        for (int row = 0; row < rows; ++row) {
            sample.values[static_cast<std::size_t>(row)] = data.at(row, col);
        }
        // BrainFlow delivers in order, but a reconnect can hand back a batch
        // that overlaps what is already buffered. Dropping the out-of-order
        // tail keeps window()'s binary search valid.
        if (!s.samples.empty() && sample.ts < s.samples.back().ts) continue;
        s.samples.push_back(std::move(sample));
    }
    trim(s);
}

void StreamRecorder::trim(Stream& s) const {
    if (s.samples.empty()) return;
    const double cutoff = s.samples.back().ts - kBufferSeconds;
    while (!s.samples.empty() && s.samples.front().ts < cutoff) {
        s.samples.pop_front();
    }
}

std::vector<Sample> StreamRecorder::window(
    BrainFlowPresets preset, double t0, double t1) const {
    std::vector<Sample> out;
    if (t1 <= t0) return out;

    std::lock_guard<std::mutex> lock(mutex_);
    const Stream& s = stream_for(preset);
    if (s.samples.empty()) return out;

    const auto begin = std::lower_bound(
        s.samples.begin(), s.samples.end(), t0,
        [](const Sample& sample, double t) { return sample.ts < t; });
    const auto end = std::lower_bound(
        begin, s.samples.end(), t1,
        [](const Sample& sample, double t) { return sample.ts < t; });

    out.assign(begin, end);
    return out;
}

std::size_t StreamRecorder::size(BrainFlowPresets preset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stream_for(preset).samples.size();
}

double StreamRecorder::latest_ts(BrainFlowPresets preset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Stream& s = stream_for(preset);
    return s.samples.empty() ? 0.0 : s.samples.back().ts;
}

void StreamRecorder::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    eeg_.samples.clear();
    imu_.samples.clear();
    ppg_.samples.clear();
}

StreamRecorder::Stream& StreamRecorder::stream_for(BrainFlowPresets preset) {
    switch (preset) {
        case BrainFlowPresets::AUXILIARY_PRESET: return imu_;
        case BrainFlowPresets::ANCILLARY_PRESET: return ppg_;
        case BrainFlowPresets::DEFAULT_PRESET:
        default:                                 return eeg_;
    }
}

const StreamRecorder::Stream& StreamRecorder::stream_for(BrainFlowPresets preset) const {
    return const_cast<StreamRecorder*>(this)->stream_for(preset);
}

}  // namespace elanora::lsl
