// The per-signal rule is what decides how much of a real session survives.
// These tests exist mainly to prove that a failure in one sensor does not
// spread to the others -- the failure mode that would quietly halve the dataset.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "elanora/data/qc.hpp"
#include "test_helpers.hpp"

using namespace elanora;
using namespace elanora::data;
using namespace elanora::test;
using Catch::Approx;

namespace {

// A plausible resting EEG channel: about 10 uV RMS of broadband activity with
// an alpha component on top.
std::vector<double> good_eeg(uint64_t seed = 5) {
    return add(make_sine(10.0, 256, 30.0, 8.0), make_noise(256, 30.0, 6.0, seed));
}

std::vector<std::vector<double>> four_good_channels() {
    return {good_eeg(1), good_eeg(2), good_eeg(3), good_eeg(4)};
}

}  // namespace

TEST_CASE("a healthy channel scores high and a flat one scores zero") {
    REQUIRE(score_eeg_channel(good_eeg(), 256) > 0.8);
    REQUIRE(score_eeg_channel(make_constant(256, 30.0, 0.0), 256) == Approx(0.0));
    REQUIRE(score_eeg_channel(make_constant(256, 30.0, 800.0), 256) == Approx(0.0));
}

TEST_CASE("a railed channel is flagged and scored zero") {
    bool excursion = false;
    std::vector<double> s = good_eeg();
    for (std::size_t i = 0; i < s.size(); i += 10) s[i] = 500.0;   // 10% railed
    REQUIRE(score_eeg_channel(s, 256, &excursion) == Approx(0.0));
    REQUIRE(excursion);
}

TEST_CASE("dropouts scale the score down instead of reading as a dead electrode") {
    // Counting NaN as zero would drag RMS toward nothing and mislabel a good
    // electrode as flat -- exactly the inversion this scorer must not make.
    std::vector<double> s = good_eeg();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (std::size_t i = 0; i < s.size() / 10; ++i) s[i] = nan;

    const double score = score_eeg_channel(s, 256);
    REQUIRE(score > 0.5);
    REQUIRE(score < score_eeg_channel(good_eeg(), 256));
}

TEST_CASE("an EEG excursion lowers only that channel") {
    std::vector<std::vector<double>> eeg = four_good_channels();
    for (std::size_t i = 0; i < eeg[1].size(); i += 8) eeg[1][i] = 400.0;

    const auto imu = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    const SignalQc qc = assess_period(eeg, make_pulse_train(1.2, 64, 30.0), imu, 256, 64, 52);

    REQUIRE(qc.eeg[0] > 0.8);
    REQUIRE(qc.eeg[1] == Approx(0.0));
    REQUIRE(qc.eeg[2] > 0.8);
    REQUIRE(qc.eeg_excursion);

    // The whole point of per-signal quality: heart and breathing are untouched.
    REQUIRE(qc.heart > 0.8);
    REQUIRE(qc.breath > 0.5);
}

TEST_CASE("head movement lowers breathing without touching EEG or heart") {
    const auto still = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    auto moving = still;
    // A 5 Hz transient: well above the respiratory band, squarely in the
    // 1-10 Hz motion band.
    moving[0] = add(moving[0], make_sine(5.0, 52, 30.0, 0.15));

    const auto eeg = four_good_channels();
    const auto ppg = make_pulse_train(1.2, 64, 30.0);

    const SignalQc a = assess_period(eeg, ppg, still, 256, 64, 52);
    const SignalQc b = assess_period(eeg, ppg, moving, 256, 64, 52);

    REQUIRE(b.motion_energy > a.motion_energy);
    REQUIRE(b.imu_motion);
    REQUIRE(b.breath < a.breath);
    REQUIRE(b.eeg[0] == Approx(a.eeg[0]).margin(1e-9));
    REQUIRE(b.heart == Approx(a.heart).margin(1e-9));
}

TEST_CASE("ordinary breathing does not trip the motion flag") {
    // The motion band starts at 1 Hz precisely so a 0.2 Hz respiratory
    // oscillation passes underneath it. If this fails, every well-measured
    // breathing period would be discarded for the movement it was measuring.
    const auto axes = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    const SignalQc qc = assess_period(four_good_channels(), make_pulse_train(1.2, 64, 30.0),
                                      axes, 256, 64, 52);
    REQUIRE_FALSE(qc.imu_motion);
    REQUIRE(qc.motion_energy < 0.25);
}

TEST_CASE("a clean period scores every signal well") {
    const auto axes = one_axis(0, make_sine(0.2, 52, 30.0, 0.05), 52, 30.0);
    const SignalQc qc = assess_period(four_good_channels(), make_pulse_train(1.2, 64, 30.0),
                                      axes, 256, 64, 52);
    for (int i = 0; i < kSensorCount; ++i) REQUIRE(qc.eeg[static_cast<std::size_t>(i)] > 0.8);
    REQUIRE(qc.heart > 0.8);
    REQUIRE(qc.breath > 0.5);
    REQUIRE_FALSE(qc.eeg_excursion);
}

TEST_CASE("missing streams score zero without disturbing the ones present") {
    const SignalQc qc = assess_period(four_good_channels(), {}, {}, 256, 64, 52);
    REQUIRE(qc.eeg[0] > 0.8);
    REQUIRE(qc.heart == Approx(0.0));
    REQUIRE(qc.breath == Approx(0.0));
}

TEST_CASE("fewer than four EEG channels leaves the rest at zero rather than reading past the end") {
    const std::vector<std::vector<double>> two = {good_eeg(1), good_eeg(2)};
    const SignalQc qc = assess_period(two, {}, {}, 256, 64, 52);
    REQUIRE(qc.eeg[0] > 0.8);
    REQUIRE(qc.eeg[2] == Approx(0.0));
    REQUIRE(qc.eeg[3] == Approx(0.0));
}
