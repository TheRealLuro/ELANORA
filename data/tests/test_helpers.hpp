#pragma once
//
// Signal builders shared by the data tests.
//
// Everything here produces a signal with a known answer, so a test can assert
// on a number rather than on "it ran". Anything that needs randomness takes an
// explicit seed: a test that fails one run in twenty is worse than no test.

#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "elanora/data/breath_features.hpp"

namespace elanora::test {

inline constexpr double kPi = 3.14159265358979323846;

// A sine of the given frequency, sample rate, and duration.
inline std::vector<double> make_sine(double hz, int fs, double seconds,
                                     double amplitude = 1.0, double phase = 0.0) {
    const int n = static_cast<int>(seconds * fs);
    std::vector<double> out(static_cast<std::size_t>(std::max(0, n)));
    for (int i = 0; i < n; ++i) {
        out[static_cast<std::size_t>(i)] =
            amplitude * std::sin(2.0 * kPi * hz * i / fs + phase);
    }
    return out;
}

inline std::vector<double> make_noise(int fs, double seconds, double sd, uint64_t seed = 1) {
    const int n = static_cast<int>(seconds * fs);
    std::vector<double> out(static_cast<std::size_t>(std::max(0, n)));
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> g(0.0, sd);
    for (double& v : out) v = g(rng);
    return out;
}

inline std::vector<double> make_constant(int fs, double seconds, double value) {
    const int n = static_cast<int>(seconds * fs);
    return std::vector<double>(static_cast<std::size_t>(std::max(0, n)), value);
}

// Adds b into a elementwise, up to the shorter length.
inline std::vector<double> add(std::vector<double> a, const std::vector<double>& b) {
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] += b[i];
    return a;
}

// A pulse train: a narrow raised-cosine bump at `hz`, which is closer to a PPG
// waveform than a sine and exercises the peak detector properly.
inline std::vector<double> make_pulse_train(double hz, int fs, double seconds,
                                            double amplitude = 1.0) {
    const int n = static_cast<int>(seconds * fs);
    const double period = fs / hz;
    const double width  = std::max(2.0, period * 0.25);
    std::vector<double> out(static_cast<std::size_t>(std::max(0, n)), 0.0);
    for (int i = 0; i < n; ++i) {
        const double ph = std::fmod(static_cast<double>(i), period);
        if (ph < width) {
            out[static_cast<std::size_t>(i)] =
                amplitude * 0.5 * (1.0 - std::cos(2.0 * kPi * ph / width));
        }
    }
    return out;
}

// Six IMU axes, all quiet noise except the named one.
inline std::array<std::vector<double>, data::kImuAxisCount>
one_axis(int index, std::vector<double> signal, int fs, double seconds,
         double noise_sd = 0.0005, uint64_t seed = 7) {
    std::array<std::vector<double>, data::kImuAxisCount> axes;
    for (int i = 0; i < data::kImuAxisCount; ++i) {
        axes[static_cast<std::size_t>(i)] =
            make_noise(fs, seconds, noise_sd, seed + static_cast<uint64_t>(i));
    }
    axes[static_cast<std::size_t>(index)] =
        add(std::move(signal), axes[static_cast<std::size_t>(index)]);
    return axes;
}

inline std::array<std::vector<double>, data::kImuAxisCount>
all_noise(int fs, double seconds, double sd, uint64_t seed = 11) {
    std::array<std::vector<double>, data::kImuAxisCount> axes;
    for (int i = 0; i < data::kImuAxisCount; ++i) {
        axes[static_cast<std::size_t>(i)] =
            make_noise(fs, seconds, sd, seed + static_cast<uint64_t>(i));
    }
    return axes;
}

}  // namespace elanora::test
