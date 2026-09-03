#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "elanora/data/heart_features.hpp"
#include "test_helpers.hpp"

using namespace elanora;
using namespace elanora::data;
using namespace elanora::test;
using Catch::Approx;

TEST_CASE("a 1.2 Hz pulse train reads as 72 BPM") {
    const HeartFeatures h = heart_from_ppg(make_pulse_train(1.2, 64, 30.0), 64);
    REQUIRE(h.bpm == Approx(72.0).margin(2.0));
    REQUIRE(h.n_beats >= 33);
    REQUIRE(h.n_beats <= 38);
    REQUIRE(h.quality > 0.8);
}

TEST_CASE("BPM tracks the pulse rate across the physiological range") {
    REQUIRE(heart_from_ppg(make_pulse_train(0.9, 64, 30.0), 64).bpm == Approx(54.0).margin(2.0));
    REQUIRE(heart_from_ppg(make_pulse_train(1.5, 64, 30.0), 64).bpm == Approx(90.0).margin(2.0));
    REQUIRE(heart_from_ppg(make_pulse_train(2.0, 64, 30.0), 64).bpm == Approx(120.0).margin(3.0));
}

TEST_CASE("a flat signal reports zero beats rather than dividing by zero") {
    const HeartFeatures h = heart_from_ppg(make_constant(64, 30.0, 512.0), 64);
    REQUIRE(h.n_beats == 0);
    REQUIRE(h.bpm == Approx(0.0));
    REQUIRE(h.quality == Approx(0.0));
}

TEST_CASE("a steady pulse has near-zero RMSSD and a variable one does not") {
    // RMSSD is a beat-to-beat difference measure, so a metronomic pulse must
    // read near zero. If it does not, the detector is jittering, not the heart.
    const HeartFeatures steady = heart_from_ppg(make_pulse_train(1.2, 64, 30.0), 64);
    REQUIRE(steady.rmssd < 25.0);

    // Alternating long and short intervals: a large beat-to-beat difference
    // with the same mean rate, which is exactly what RMSSD exists to catch.
    std::vector<double> irregular(64 * 30, 0.0);
    double t = 0.5;
    bool longer = false;
    while (t < 29.0) {
        const auto i = static_cast<std::size_t>(t * 64);
        if (i + 4 < irregular.size()) {
            for (int k = 0; k < 4; ++k) irregular[i + static_cast<std::size_t>(k)] = 1.0;
        }
        t += longer ? 1.0 : 0.6;
        longer = !longer;
    }
    const HeartFeatures var = heart_from_ppg(irregular, 64);
    REQUIRE(var.rmssd > steady.rmssd);
    REQUIRE(var.rmssd > 100.0);
}

TEST_CASE("implausible intervals are rejected before averaging") {
    // A burst of detections 50 ms apart is not a 1200 BPM heart. Averaging them
    // in would drag BPM up and RMSSD with it.
    std::vector<double> s = make_pulse_train(1.2, 64, 30.0);
    for (int k = 0; k < 6; ++k) {
        s[static_cast<std::size_t>(600 + k * 3)] = 3.0;
    }
    const HeartFeatures h = heart_from_ppg(s, 64);
    REQUIRE(h.bpm > 50.0);
    REQUIRE(h.bpm < 100.0);
}

TEST_CASE("noise scores low quality rather than a confident wrong rate") {
    const HeartFeatures h = heart_from_ppg(make_noise(64, 30.0, 1.0, 3), 64);
    REQUIRE(h.quality < 0.6);
}

TEST_CASE("a short or empty record is refused") {
    REQUIRE(heart_from_ppg({}, 64).n_beats == 0);
    REQUIRE(heart_from_ppg(make_pulse_train(1.2, 64, 30.0), 0).n_beats == 0);
}
