#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <limits>
#include <random>
#include <vector>

#include "elanora/device/signal_quality.hpp"

using namespace elanora::device;

namespace {

constexpr int kSr = 256;

// amplitude_uv is peak amplitude; a sine's RMS is amplitude / sqrt(2).
std::vector<double> sine(double freq_hz, double amplitude_uv, double seconds) {
    const int n = static_cast<int>(kSr * seconds);
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            amplitude_uv * std::sin(2.0 * std::numbers::pi * freq_hz * i / kSr);
    }
    return v;
}

}  // namespace

TEST_CASE("a flat trace reads as flat and bad", "[quality]") {
    // A disconnected or shorted electrode. Still "a signal" -- just not one
    // that means anything.
    const std::vector<double> flat(kSr, 42.0);
    const auto q = assess(flat, kSr);
    REQUIRE(q.flat);
    REQUIRE(q.q == Quality::Bad);
}

TEST_CASE("a railed trace reads as railed and bad", "[quality]") {
    std::vector<double> square(kSr);
    for (int i = 0; i < kSr; ++i) {
        square[static_cast<std::size_t>(i)] = (i % 2 == 0) ? 500.0 : -500.0;
    }
    const auto q = assess(square, kSr);
    REQUIRE(q.railed);
    REQUIRE(q.q == Quality::Bad);
}

TEST_CASE("a realistic 10 Hz alpha trace reads as good", "[quality]") {
    // 14 uV peak -> ~10 uV RMS, squarely in resting scalp EEG range.
    const auto q = assess(sine(10.0, 14.0, 1.0), kSr);
    REQUIRE_FALSE(q.flat);
    REQUIRE_FALSE(q.railed);
    REQUIRE(q.rms_uv == Catch::Approx(10.0).margin(1.0));
    REQUIRE(q.q == Quality::Good);
}

TEST_CASE("a large but unsaturated trace reads as fair, not bad", "[quality]") {
    // ~99 uV RMS: dominated by artifact but still recoverable information.
    // Collapsing this to Bad would throw away usable trials.
    const auto q = assess(sine(10.0, 140.0, 1.0), kSr);
    REQUIRE_FALSE(q.railed);
    REQUIRE(q.q == Quality::Fair);
}

TEST_CASE("electrode drift alone does not make a good channel look railed", "[quality]") {
    // A slow ramp across the window plus normal EEG. Without detrending the
    // ramp dominates RMS and a perfectly good electrode is rejected.
    auto v = sine(10.0, 14.0, 1.0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] += 0.4 * static_cast<double>(i);   // ramps to ~100 uV
    }
    const auto q = assess(v, kSr);
    REQUIRE(q.rms_uv == Catch::Approx(10.0).margin(2.0));
    REQUIRE(q.q == Quality::Good);
}

TEST_CASE("a near-silent channel is bad even though it is not flat", "[quality]") {
    // Attached but barely picking anything up -- as unusable as no contact.
    const auto q = assess(sine(10.0, 0.5, 1.0), kSr);
    REQUIRE_FALSE(q.flat);
    REQUIRE(q.q == Quality::Bad);
}

TEST_CASE("NaN or infinite samples are bad, never propagated", "[quality]") {
    // A dropped BLE packet can surface as NaN. Letting it through would make
    // every downstream band power NaN with no indication of the cause.
    std::vector<double> v = sine(10.0, 14.0, 1.0);
    v[100] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(assess(v, kSr).q == Quality::Bad);

    v[100] = std::numeric_limits<double>::infinity();
    REQUIRE(assess(v, kSr).q == Quality::Bad);
}

TEST_CASE("a too-short window is bad rather than a guess", "[quality]") {
    REQUIRE(assess({}, kSr).q == Quality::Bad);
    REQUIRE(assess({1.0, 2.0, 3.0}, kSr).q == Quality::Bad);
}
