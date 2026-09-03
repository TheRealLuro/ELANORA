#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <numeric>

#include "elanora/data/eeg_features.hpp"
#include "test_helpers.hpp"

using namespace elanora;
using namespace elanora::data;
using namespace elanora::test;
using Catch::Approx;

namespace {
constexpr int kAlpha = index_of(Band::Alpha);
constexpr int kBeta  = index_of(Band::Beta);
constexpr int kTheta = index_of(Band::Theta);
constexpr int kDelta = index_of(Band::Delta);
constexpr int kGamma = index_of(Band::Gamma);
}  // namespace

TEST_CASE("a pure 10 Hz sine is dominated by alpha") {
    const BandPowers bp = band_powers(make_sine(10.0, 256, 30.0), 256);
    REQUIRE(bp.rel[kAlpha] > 0.5);
    REQUIRE(bp.rel[kAlpha] > bp.rel[kBeta]);
}

TEST_CASE("relative powers sum to one and absolute powers do not") {
    const BandPowers bp = band_powers(make_sine(10.0, 256, 30.0, 4.0), 256);
    const double rel_sum = std::accumulate(bp.rel.begin(), bp.rel.end(), 0.0);
    REQUIRE(rel_sum == Approx(1.0).margin(0.02));
    // Amplitude 4 gives mean square 8, so the absolute total is nowhere near 1.
    REQUIRE(std::accumulate(bp.abs.begin(), bp.abs.end(), 0.0) > 1.0);
}

TEST_CASE("a 6 Hz sine lands in theta, proving band edges are not off by a bin") {
    REQUIRE(band_powers(make_sine(6.0, 256, 30.0), 256).rel[kTheta] > 0.5);
}

TEST_CASE("each band captures a tone placed inside it") {
    // One test per edge pair. An off-by-one in the edge table would show up as
    // a tone landing in its neighbour, which no single-band test would catch.
    REQUIRE(band_powers(make_sine(2.0,  256, 30.0), 256).rel[kDelta] > 0.5);
    REQUIRE(band_powers(make_sine(6.0,  256, 30.0), 256).rel[kTheta] > 0.5);
    REQUIRE(band_powers(make_sine(10.0, 256, 30.0), 256).rel[kAlpha] > 0.5);
    REQUIRE(band_powers(make_sine(20.0, 256, 30.0), 256).rel[kBeta]  > 0.5);
    REQUIRE(band_powers(make_sine(38.0, 256, 30.0), 256).rel[kGamma] > 0.5);
}

TEST_CASE("doubling amplitude changes absolute power but not relative") {
    // This is the test that justifies storing both representations. Relative
    // power is blind to an amplitude change; absolute power tracks it exactly.
    // Neither alone can tell "alpha rose" from "everything else fell".
    const BandPowers a = band_powers(make_sine(10.0, 256, 30.0, 1.0), 256);
    const BandPowers b = band_powers(make_sine(10.0, 256, 30.0, 2.0), 256);

    REQUIRE(b.abs[kAlpha] > a.abs[kAlpha] * 3.0);
    REQUIRE(b.rel[kAlpha] == Approx(a.rel[kAlpha]).margin(0.02));
}

TEST_CASE("60 Hz mains does not leak into gamma") {
    // Gamma is capped at 45 Hz specifically so mains and its filter skirt stay
    // out. A strong 60 Hz component must barely register.
    const std::vector<double> clean = make_sine(10.0, 256, 30.0, 1.0);
    const std::vector<double> hummy = add(clean, make_sine(60.0, 256, 30.0, 5.0));

    const BandPowers a = band_powers(clean, 256);
    const BandPowers b = band_powers(hummy, 256);
    REQUIRE(b.abs[kGamma] == Approx(a.abs[kGamma]).margin(0.05 * a.total + 1e-6));
}

TEST_CASE("slow drift does not pile into delta") {
    std::vector<double> s = make_sine(10.0, 256, 30.0, 1.0);
    for (std::size_t i = 0; i < s.size(); ++i) s[i] += 0.02 * static_cast<double>(i);
    const BandPowers bp = band_powers(s, 256);
    REQUIRE(bp.rel[kAlpha] > 0.5);
    REQUIRE(bp.rel[kDelta] < 0.2);
}

TEST_CASE("too short or malformed input returns zeros rather than noise") {
    const BandPowers a = band_powers({1.0, 2.0, 3.0}, 256);
    REQUIRE(a.total == Approx(0.0));
    const BandPowers b = band_powers(make_sine(10.0, 256, 30.0), 0);
    REQUIRE(b.total == Approx(0.0));
}

// ---------------------------------------------------------------------------
// Dominance
// ---------------------------------------------------------------------------

namespace {
BandPowers with_rel(std::array<double, kBandCount> rel) {
    BandPowers bp{};
    bp.rel = rel;
    bp.total = 1.0;
    for (int i = 0; i < kBandCount; ++i) bp.abs[static_cast<std::size_t>(i)] = rel[static_cast<std::size_t>(i)];
    return bp;
}
}  // namespace

TEST_CASE("a clear leader is reported alone with its margin") {
    const Dominance d = assess_dominance(with_rel({0.10, 0.20, 0.60, 0.05, 0.05}));
    REQUIRE(d.dominant == Band::Alpha);
    REQUIRE(d.margin == Approx(0.40));
    REQUIRE(d.codominant.size() == 1u);
    REQUIRE(d.codominant[0] == Band::Alpha);
}

TEST_CASE("a near tie reports both bands rather than inventing a winner") {
    // 0.02 between alpha and theta is not a finding about alpha. Forcing a
    // single label here would be the classifier asserting more than the data.
    const Dominance d = assess_dominance(with_rel({0.08, 0.40, 0.42, 0.05, 0.05}));
    REQUIRE(d.dominant == Band::Alpha);
    REQUIRE(d.margin == Approx(0.02).margin(1e-9));
    REQUIRE(d.codominant.size() == 2u);
    REQUIRE(d.codominant[0] == Band::Theta);
    REQUIRE(d.codominant[1] == Band::Alpha);
}

TEST_CASE("the co-dominance threshold is what decides how many bands are named") {
    const BandPowers bp = with_rel({0.08, 0.40, 0.42, 0.05, 0.05});
    REQUIRE(assess_dominance(bp, 0.01).codominant.size() == 1u);
    REQUIRE(assess_dominance(bp, 0.05).codominant.size() == 2u);
}

TEST_CASE("an all-equal spectrum names every band, margin zero") {
    const Dominance d = assess_dominance(with_rel({0.2, 0.2, 0.2, 0.2, 0.2}));
    REQUIRE(d.margin == Approx(0.0));
    REQUIRE(d.codominant.size() == 5u);
}
