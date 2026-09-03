#include "elanora/collector/audio_engine.hpp"

#include <atomic>
#include <mutex>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 4244 4267 4100)
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace elanora::collector {

struct AudioEngine::Impl {
    ::ma_device device{};
    bool      device_ok = false;
    ToneGenerator gen{48000};

    // The generator is reconfigured from the UI thread between rounds while
    // the audio thread is rendering. A mutex in an audio callback is normally
    // wrong, but this one is uncontended for all but a few microseconds once
    // per 120 seconds, and the alternative -- a lock-free swap of the whole
    // generator -- is far more machinery than the problem deserves.
    std::mutex mutex;
};

// Free function, so it needs access to Impl -- which is private. Declared as a
// static member instead of a friend: fewer moving parts, same result.
void AudioEngine::data_callback(::ma_device* device, void* output, const void* input,
                                unsigned int frames) {
    (void)input;
    auto* impl = static_cast<Impl*>(device->pUserData);
    if (impl == nullptr) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->gen.render(static_cast<float*>(output), static_cast<int>(frames));
}

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(std::string& err) {
    err.clear();
    if (impl_->device_ok) return true;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 48000;
    cfg.dataCallback      = &AudioEngine::data_callback;
    cfg.pUserData         = impl_.get();

    if (ma_device_init(nullptr, &cfg, &impl_->device) != MA_SUCCESS) {
        err = "no audio output device";
        return false;
    }
    // The device may negotiate a different rate than requested; the generator
    // must follow it or every stimulus frequency is wrong by that ratio.
    impl_->gen.set_sample_rate(static_cast<int>(impl_->device.sampleRate));

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        err = "could not start audio output";
        return false;
    }
    impl_->device_ok = true;
    return true;
}

void AudioEngine::stop() {
    if (!impl_ || !impl_->device_ok) return;
    ma_device_uninit(&impl_->device);
    impl_->device_ok = false;
}

bool AudioEngine::running() const { return impl_ && impl_->device_ok; }

void AudioEngine::set_gate(bool open) {
    if (impl_) impl_->gen.set_gate(open);
}

void AudioEngine::configure(const StimulusDesign& design, Condition cond, double hz,
                            double jitter_mean_hz, uint64_t seed) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->gen.configure(design, cond, hz, jitter_mean_hz, seed);
}

int AudioEngine::sample_rate() const {
    return impl_ ? impl_->gen.sample_rate() : 48000;
}

}  // namespace elanora::collector
