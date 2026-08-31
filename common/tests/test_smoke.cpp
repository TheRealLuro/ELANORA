#include <catch2/catch_test_macros.hpp>

// Proves the test harness itself is wired up: Catch2 fetched, linked, and
// discovered by CTest. If this fails, nothing else in the repo is meaningful.
TEST_CASE("test harness runs", "[smoke]") {
    REQUIRE(1 + 1 == 2);
}
