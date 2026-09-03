#pragma once
//
// Audio output for the stimulus. Owns a miniaudio device whose callback drives
// a ToneGenerator.
//
// The device stays open for the whole trial and the gate is what changes; the
// alternative -- starting and stopping the device per round -- costs tens of
// milliseconds of driver latency at exactly the moment the stimulus onset has
// to be accurate.

#include <memory>
#include <string>

#include "elanora/collector/tone.hpp"

// Forward declared at GLOBAL scope on purpose. Writing "struct ma_device*" in
// a member declaration inside the namespace would declare
// elanora::collector::ma_device, a different type from miniaudio's, and the
// callback would then fail to convert.
struct ma_device;

namespace elanora::collector {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Opens the default output device. Returns false with a reason rather than
    // throwing: no speaker is a normal condition on a lab machine, and the
    // trial should still be runnable silently to check the protocol.
    bool start(std::string& err);
    void stop();
    bool running() const;

    // Safe from the UI thread.
    void set_gate(bool open);
    void configure(const StimulusDesign& design, Condition cond, double hz,
                   double jitter_mean_hz, uint64_t seed);

    int sample_rate() const;

private:
    struct Impl;
    // miniaudio calls this from the audio thread; it needs to reach Impl, which
    // is private, so it is a member rather than a free function.
    static void data_callback(::ma_device* device, void* output,
                              const void* input, unsigned int frames);
    std::unique_ptr<Impl> impl_;
};

}  // namespace elanora::collector
