#include "elanora/collector/tone.hpp"

#include <algorithm>
#include <cmath>

namespace elanora::collector {

namespace {
constexpr double kTau = 6.283185307179586;

// Continuous carrier is scaled so it matches a gated tone in loudness.
//
// A 50%-duty gated sine of amplitude A has RMS A/2; a continuous sine of the
// same amplitude has RMS A/sqrt(2). Without this factor the tone control is
// simply louder than every stimulus round, and loudness becomes the difference
// the control was supposed to rule out.
constexpr double kContinuousScale = 0.70710678118654752;
}  // namespace

ToneGenerator::ToneGenerator(int sample_rate) { set_sample_rate(sample_rate); }

void ToneGenerator::set_sample_rate(int sr) {
    sr_ = std::max(8000, sr);
    // ~4 ms ramp. Long enough to remove the click, short enough that the pulse
    // edge is still sharp at 45 Hz, where a full period is only 22 ms.
    const double ramp_s = 0.004;
    ramp_coeff_ = 1.0 - std::exp(-1.0 / (ramp_s * sr_));
}

void ToneGenerator::configure(const StimulusDesign& design, Condition cond, double hz,
                              double jitter_mean_hz, uint64_t seed) {
    cond_ = cond;
    mode_ = design.mode;
    carrier_hz_ = design.carrier_hz;
    duty_ = std::clamp(design.duty, 0.05, 0.95);
    rng_ = seed ? seed : 1;

    n_layers_ = 0;
    for (int i = 0; i < kMaxLayers; ++i) { rate_[i] = 0.0; amp_[i] = 0.0; gate_phase_[i] = 0.0; }

    if (cond == Condition::Stim) {
        if (mode_ == StimMode::Stacked) {
            for (const auto& l : design.layers) {
                if (!l.enabled || n_layers_ >= kMaxLayers) continue;
                rate_[n_layers_] = l.hz;
                amp_[n_layers_] = l.amp;
                ++n_layers_;
            }
        } else if (hz > 0.0) {
            rate_[0] = hz;
            amp_[0] = 1.0;
            n_layers_ = 1;
        }
    } else if (cond == Condition::ControlJitter) {
        jitter_mean_hz_ = jitter_mean_hz > 0.0 ? jitter_mean_hz : 10.0;
        jitter_width_ = duty_ / jitter_mean_hz_;
        jitter_t_ = 0.0;
        jitter_next_ = next_jitter_gap();
        jitter_on_ = true;
    }

    carrier_phase_ = 0.0;
    env_ = 0.0;
}

double ToneGenerator::next_jitter_gap() {
    // xorshift, so the sequence is reproducible from the seed and the control
    // can be regenerated exactly when reviewing a recording.
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
    const double u = static_cast<double>(rng_ % 1000000u) / 1000000.0;
    // This is the OFF time, not the whole period. Returning a full period here
    // made the real period gap + width, so a 10 Hz jitter control ran at 6.7
    // pulses per second and no longer matched the stimulus rounds on pulse
    // count -- which is the one thing the control has to hold constant.
    //
    // +/-45% around the mean, so no consistent rate survives while the count
    // still matches a stimulus round of the same mean rate.
    return (1.0 / jitter_mean_hz_) * (1.0 - duty_) * (0.55 + 0.9 * u);
}

void ToneGenerator::render(float* out, int frames) {
    const double carrier_inc = kTau * carrier_hz_ / sr_;
    const double dt = 1.0 / sr_;
    const bool open = gate_;

    for (int i = 0; i < frames; ++i) {
        double target = 0.0;

        if (open) {
            if (cond_ == Condition::ControlTone) {
                target = kContinuousScale;
            } else if (cond_ == Condition::ControlJitter) {
                jitter_t_ += dt;
                if (!jitter_on_ && jitter_t_ >= jitter_next_) {
                    jitter_on_ = true;
                    jitter_t_ = 0.0;
                } else if (jitter_on_ && jitter_t_ >= jitter_width_) {
                    jitter_on_ = false;
                    jitter_t_ = 0.0;
                    jitter_next_ = next_jitter_gap();
                }
                target = jitter_on_ ? 1.0 : 0.0;
            } else if (n_layers_ > 0) {
                // Sum the gates and divide by the layer count, so stacking can
                // never clip and adding a layer never raises the level.
                double sum = 0.0;
                for (int l = 0; l < n_layers_; ++l) {
                    gate_phase_[l] += rate_[l] * dt;
                    if (gate_phase_[l] >= 1.0) gate_phase_[l] -= 1.0;
                    sum += (gate_phase_[l] < duty_ ? 1.0 : 0.0) * amp_[l];
                }
                target = sum / n_layers_;
            }
        }

        // One-pole follower toward the target, which rounds every gate edge.
        env_ += (target - env_) * ramp_coeff_;

        carrier_phase_ += carrier_inc;
        if (carrier_phase_ >= kTau) carrier_phase_ -= kTau;

        const float s = static_cast<float>(std::sin(carrier_phase_) * env_ * amplitude_);
        out[i * 2]     = s;
        out[i * 2 + 1] = s;
    }
}

double ToneGenerator::rms(const float* buf, int frames) {
    if (frames <= 0) return 0.0;
    double acc = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double v = buf[i * 2];
        acc += v * v;
    }
    return std::sqrt(acc / frames);
}

}  // namespace elanora::collector
