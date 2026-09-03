#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "elanora/collector/session.hpp"
#include "elanora/csv.hpp"
#include "elanora/server/schedule_json.hpp"

using namespace elanora;
using namespace elanora::collector;

namespace {

int count(const std::string& s, char c) {
    return static_cast<int>(std::count(s.begin(), s.end(), c));
}

std::vector<PlannedRound> default_rounds(uint64_t seed) {
    const auto freqs = geometric_set(kProtocolFreqLo, kProtocolFreqHi, kProtocolFreqCount);
    return build_schedule(freqs, kProtocolJitter, kProtocolTone, seed);
}

}  // namespace

TEST_CASE("the schedule JSON carries every round", "[server][schedule]") {
    const std::string js = server::schedule_json(default_rounds(7));

    REQUIRE(js.find("\"rounds\"") != std::string::npos);
    // One outer object plus one per round.
    REQUIRE(count(js, '{') == kProtocolRounds + 1);
    REQUIRE(count(js, '}') == kProtocolRounds + 1);
}

TEST_CASE("both control conditions survive into the JSON", "[server][schedule]") {
    // The phone shows the operator a neutral label, but the condition still has
    // to reach the trial row -- an unlabelled trial has no place in the dataset.
    const std::string js = server::schedule_json(default_rounds(7));
    REQUIRE(js.find("control_jitter") != std::string::npos);
    REQUIRE(js.find("control_tone") != std::string::npos);
    REQUIRE(js.find("\"stim\"") != std::string::npos);
}

TEST_CASE("the JSON is a faithful rendering of build_schedule", "[server][schedule]") {
    // The point of the endpoint is that the phone never owns a second copy of
    // the randomisation rules. If this drifts, it owns one by accident.
    const auto rounds = default_rounds(1234);
    const std::string js = server::schedule_json(rounds);

    for (std::size_t i = 0; i < rounds.size(); ++i) {
        const std::string idx = "\"index\":" + std::to_string(i + 1);
        REQUIRE(js.find(idx) != std::string::npos);
    }
    REQUIRE(js.find(fmt6(rounds.front().hz)) != std::string::npos);
}

TEST_CASE("the same seed renders the same schedule", "[server][schedule]") {
    REQUIRE(server::schedule_json(default_rounds(99)) ==
            server::schedule_json(default_rounds(99)));
    REQUIRE(server::schedule_json(default_rounds(99)) !=
            server::schedule_json(default_rounds(100)));
}

TEST_CASE("an empty schedule is still valid JSON", "[server][schedule]") {
    REQUIRE(server::schedule_json({}) == "{\"rounds\":[]}");
}
