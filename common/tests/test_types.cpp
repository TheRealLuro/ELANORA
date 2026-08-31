#include <catch2/catch_test_macros.hpp>

#include "elanora/types.hpp"

using namespace elanora;

TEST_CASE("sensor ids map to the fixed Muse 2 electrode layout", "[types]") {
    // This mapping is hardware-defined and appears in every CSV ever written.
    // If it changes, every dataset on disk becomes mislabelled.
    REQUIRE(std::string(electrode_name(SensorId::S1)) == "TP9");
    REQUIRE(std::string(electrode_name(SensorId::S2)) == "AF7");
    REQUIRE(std::string(electrode_name(SensorId::S3)) == "AF8");
    REQUIRE(std::string(electrode_name(SensorId::S4)) == "TP10");
}

TEST_CASE("band edges are contiguous and stop below mains noise", "[types]") {
    // Gamma must end at 45 Hz, not 50: above that the 60 Hz mains peak and the
    // anti-alias filter skirt dominate whatever real signal is present.
    for (int i = 0; i + 1 < kBandCount; ++i) {
        REQUIRE(kBandEdges[i][1] == kBandEdges[i + 1][0]);
    }
    REQUIRE(kBandEdges[0][0] == kAnalysisLowHz);
    REQUIRE(kBandEdges[kBandCount - 1][1] == kAnalysisHighHz);
    REQUIRE(kAnalysisHighHz < 60.0);
}

TEST_CASE("condition names round-trip through parsing", "[types]") {
    for (auto c : {Condition::Stim, Condition::ControlJitter, Condition::ControlTone}) {
        Condition parsed{};
        REQUIRE(parse_condition(condition_name(c), parsed));
        REQUIRE(parsed == c);
    }
    Condition unused{};
    REQUIRE_FALSE(parse_condition("not_a_condition", unused));
}

TEST_CASE("period names round-trip through parsing", "[types]") {
    for (auto p : {Period::Baseline, Period::Stimulus, Period::Post}) {
        Period parsed{};
        REQUIRE(parse_period(period_name(p), parsed));
        REQUIRE(parsed == p);
    }
}

TEST_CASE("band names are the exact spellings used in CSV columns", "[types]") {
    REQUIRE(std::string(band_name(Band::Delta)) == "delta");
    REQUIRE(std::string(band_name(Band::Theta)) == "theta");
    REQUIRE(std::string(band_name(Band::Alpha)) == "alpha");
    REQUIRE(std::string(band_name(Band::Beta))  == "beta");
    REQUIRE(std::string(band_name(Band::Gamma)) == "gamma");
}
