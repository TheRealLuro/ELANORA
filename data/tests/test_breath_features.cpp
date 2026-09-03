#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "elanora/data/breath_features.hpp"
#include "test_helpers.hpp"

using namespace elanora;
using namespace elanora::data;
using namespace elanora::test;
using Catch::Approx;

TEST_CASE("a 0.2 Hz oscillation reads as 12 breaths per minute with high confidence") {
    const auto axes = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    const BreathFeatures b = breath_from_imu(axes, 52);

    REQUIRE(b.breaths_per_minute == Approx(12.0).margin(1.0));
    REQUIRE(b.axis_used == "ax");
    REQUIRE(b.confidence > 0.7);
}

TEST_CASE("the reported axis is the one carrying the breathing") {
    // Which axis breathing lands on depends on how the headset sits, so the
    // choice cannot be fixed in advance -- and recording it makes the choice
    // auditable rather than invisible.
    for (int axis : {0, 2, 4}) {
        const auto axes = one_axis(axis, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
        REQUIRE(breath_from_imu(axes, 52).axis_used == std::string(imu_axis_name(axis)));
    }
}

TEST_CASE("breathing rate tracks the oscillation across the resting range") {
    // 0.13 Hz is about 8 breaths/min, 0.33 Hz about 20.
    const auto slow = one_axis(1, make_sine(0.1333, 52, 30.0, 0.05), 52, 30.0);
    const auto fast = one_axis(1, make_sine(0.3333, 52, 30.0, 0.05), 52, 30.0);
    REQUIRE(breath_from_imu(slow, 52).breaths_per_minute == Approx(8.0).margin(1.5));
    REQUIRE(breath_from_imu(fast, 52).breaths_per_minute == Approx(20.0).margin(1.5));
}

TEST_CASE("pure noise reports low confidence rather than a confident wrong number") {
    const BreathFeatures b = breath_from_imu(all_noise(52, 30.0, 0.01), 52);
    REQUIRE(b.confidence < 0.4);
}

TEST_CASE("the two estimators agreeing raises confidence above either disagreeing") {
    // Confidence has to be driven by cross-method agreement, not amplitude
    // alone -- a large artifact is a large signal that both estimators read
    // differently, and that difference is the only warning available.
    const auto clean = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    const auto messy = one_axis(
        0, add(make_sine(0.2, 52, 30.0, 0.05), make_sine(0.37, 52, 30.0, 0.05)), 52, 30.0);

    REQUIRE(breath_from_imu(clean, 52).confidence >
            breath_from_imu(messy, 52).confidence);
}

TEST_CASE("motion energy discounts the estimate directly") {
    // A head turn produces a large slow component both estimators will read as
    // breathing. Motion has to cut the confidence or a moving subject looks
    // like a well-measured one.
    const auto axes = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    const double still  = breath_from_imu(axes, 52, 0.0).confidence;
    const double moving = breath_from_imu(axes, 52, 0.8).confidence;
    REQUIRE(moving < still);
    REQUIRE(moving == Approx(still * 0.2).epsilon(1e-6));
}

TEST_CASE("the spectral cross-check agrees with the interval estimate on clean input") {
    const auto axes = one_axis(0, make_sine(0.25, 52, 30.0, 0.05), 52, 30.0);
    const BreathFeatures b = breath_from_imu(axes, 52);
    REQUIRE(b.bpm_spectral == Approx(15.0).margin(1.5));
    REQUIRE(b.breaths_per_minute == Approx(b.bpm_spectral).margin(2.0));
}

TEST_CASE("too few cycles caps confidence even when the signal is clean") {
    // 8 s at 0.2 Hz is under 2 cycles. The estimate may be right, but the
    // record cannot support calling it confident.
    const auto axes = one_axis(0, make_sine(0.2, 52, 8.0, 0.05), 52, 8.0);
    REQUIRE(breath_from_imu(axes, 52).confidence < 0.5);
}

TEST_CASE("empty or malformed input returns no axis and zero confidence") {
    std::array<std::vector<double>, kImuAxisCount> empty;
    const BreathFeatures b = breath_from_imu(empty, 52);
    REQUIRE(b.axis_used.empty());
    REQUIRE(b.confidence == Approx(0.0));

    const auto axes = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    REQUIRE(breath_from_imu(axes, 0).confidence == Approx(0.0));
}
