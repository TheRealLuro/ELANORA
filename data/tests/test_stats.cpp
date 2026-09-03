#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

#include "elanora/data/stats.hpp"

using namespace elanora;
using namespace elanora::data;
using Catch::Approx;

TEST_CASE("the incomplete beta matches values that can be checked by hand") {
    // I_x(1,1) = x, and I_0.5(a,a) = 0.5 by symmetry. If the continued fraction
    // is wrong, every p-value in the project is wrong with it.
    REQUIRE(incomplete_beta(1.0, 1.0, 0.25) == Approx(0.25).margin(1e-10));
    REQUIRE(incomplete_beta(1.0, 1.0, 0.80) == Approx(0.80).margin(1e-10));
    REQUIRE(incomplete_beta(3.0, 3.0, 0.50) == Approx(0.50).margin(1e-10));
    REQUIRE(incomplete_beta(0.5, 0.5, 0.50) == Approx(0.50).margin(1e-10));
    REQUIRE(incomplete_beta(2.0, 1.0, 0.50) == Approx(0.25).margin(1e-10));
    REQUIRE(incomplete_beta(2.0, 2.0, 0.0) == Approx(0.0));
    REQUIRE(incomplete_beta(2.0, 2.0, 1.0) == Approx(1.0));
}

TEST_CASE("the t distribution tail matches published critical values") {
    // Two-sided 0.05 critical values, from any t table.
    REQUIRE(t_test_p(2.228, 10.0) == Approx(0.05).margin(0.001));
    REQUIRE(t_test_p(2.086, 20.0) == Approx(0.05).margin(0.001));
    REQUIRE(t_test_p(1.960, 1e7)  == Approx(0.05).margin(0.001));
    REQUIRE(t_test_p(0.0, 10.0)   == Approx(1.0).margin(1e-9));
    // Symmetric in the sign of t.
    REQUIRE(t_test_p(-2.228, 10.0) == Approx(t_test_p(2.228, 10.0)).margin(1e-12));
}

TEST_CASE("Welch's t on identical samples gives p near one") {
    const std::vector<double> a = {1, 2, 3, 4, 5, 6, 7, 8};
    const WelchT w = welch_t_test(a, a);
    REQUIRE(w.t == Approx(0.0).margin(1e-12));
    REQUIRE(w.p == Approx(1.0).margin(1e-9));
}

TEST_CASE("Welch's t separates well-separated samples") {
    const std::vector<double> a = {10, 11, 12, 10.5, 11.5, 10.2, 11.8, 10.9};
    const std::vector<double> b = {1, 2, 1.5, 1.2, 0.8, 1.9, 1.1, 1.4};
    const WelchT w = welch_t_test(a, b);
    REQUIRE(w.p < 0.01);
    REQUIRE(w.t > 0.0);
    REQUIRE(w.difference() > 8.0);
}

TEST_CASE("Welch uses unequal-variance degrees of freedom") {
    // Groups with very different variances must not get the pooled df of
    // n_a + n_b - 2 = 18; Welch's correction pulls it well below that.
    const std::vector<double> tight = {5.0, 5.1, 4.9, 5.0, 5.1, 4.9, 5.0, 5.0, 5.1, 4.9};
    const std::vector<double> loose = {1, 9, 2, 8, 3, 7, 4, 6, 0, 10};
    const WelchT w = welch_t_test(tight, loose);
    REQUIRE(w.df < 12.0);
    REQUIRE(w.df > 8.0);
}

TEST_CASE("a group too small to test returns p of one rather than a division by zero") {
    REQUIRE(welch_t_test(std::vector<double>{1.0}, std::vector<double>{2.0, 3.0}).p
            == Approx(1.0));
    REQUIRE(welch_t_test({}, {}).p == Approx(1.0));

    // Two constant groups: zero variance, no test possible.
    const std::vector<double> ca(5, 3.0);
    const std::vector<double> cb(5, 9.0);
    REQUIRE(welch_t_test(ca, cb).p == Approx(1.0));
}

TEST_CASE("Benjamini-Hochberg matches the hand-computed correction") {
    // p * n / rank, then enforced monotone from the largest downward.
    //   0.001*5/1=0.005  0.008*5/2=0.020  0.039*5/3=0.065
    //   0.041*5/4=0.05125 -> pulled down to 0.05125, which then caps 0.065
    const std::vector<double> raw = {0.001, 0.008, 0.039, 0.041, 0.9};
    const std::vector<double> adj = benjamini_hochberg(raw);

    REQUIRE(adj[0] == Approx(0.005).margin(1e-9));
    REQUIRE(adj[1] == Approx(0.020).margin(1e-9));
    REQUIRE(adj[3] == Approx(0.05125).margin(1e-9));
    REQUIRE(adj[2] == Approx(0.05125).margin(1e-9));
    REQUIRE(adj[4] == Approx(0.9).margin(1e-9));
}

TEST_CASE("the correction is monotone and never below the raw value") {
    std::mt19937_64 rng(9);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> raw(22);
    for (double& v : raw) v = u(rng);

    const std::vector<double> adj = benjamini_hochberg(raw);
    for (std::size_t i = 0; i < raw.size(); ++i) {
        REQUIRE(adj[i] >= raw[i] - 1e-12);
        REQUIRE(adj[i] <= 1.0);
    }
    // Sorting by raw p must leave the corrected values non-decreasing.
    std::vector<std::size_t> order(raw.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](std::size_t i, std::size_t j) { return raw[i] < raw[j]; });
    for (std::size_t k = 1; k < order.size(); ++k) {
        REQUIRE(adj[order[k]] >= adj[order[k - 1]] - 1e-12);
    }
}

TEST_CASE("identical p-values are scaled by n over rank") {
    const std::vector<double> raw(4, 0.02);
    const std::vector<double> adj = benjamini_hochberg(raw);
    // The running minimum from the largest rank down gives 0.02*4/4 = 0.02 for
    // all four.
    for (double v : adj) REQUIRE(v == Approx(0.02).margin(1e-12));
}

TEST_CASE("BH on 22 null outcomes rejects far less often than uncorrected testing") {
    // The reason the correction exists. With 22 outcomes at alpha 0.05 you
    // expect about one false positive per run by chance, so an uncorrected gate
    // would pass on pure noise most of the time.
    std::mt19937_64 rng(4);
    std::uniform_real_distribution<double> u(0.0, 1.0);

    int raw_hits = 0, adj_hits = 0;
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<double> p(22);
        for (double& v : p) v = u(rng);
        const std::vector<double> a = benjamini_hochberg(p);
        if (std::any_of(p.begin(), p.end(), [](double v) { return v < 0.05; })) ++raw_hits;
        if (std::any_of(a.begin(), a.end(), [](double v) { return v < 0.05; })) ++adj_hits;
    }
    REQUIRE(raw_hits > 250);   // uncorrected: nearly always something "significant"
    REQUIRE(adj_hits < 60);    // corrected: close to the nominal 5%
}

TEST_CASE("a permutation test on random data returns a p that is not tiny") {
    std::mt19937_64 seed_rng(2);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<double> values(40);
    for (double& v : values) v = u(seed_rng);

    // The statistic ignores the shuffling, so the observed value sits squarely
    // inside its own null and p must land near 1.
    auto stat = [&]() { return mean(values); };
    auto shuffle = [&](std::mt19937_64& rng) { std::shuffle(values.begin(), values.end(), rng); };

    const PermutationResult r = permutation_test(stat, shuffle, mean(values), 200, 1);
    REQUIRE(r.p > 0.9);
    REQUIRE(r.n_perm == 200);
}

TEST_CASE("the permutation p can never be exactly zero") {
    // A finite permutation set cannot justify p = 0; the floor is 1/(n+1).
    std::vector<double> v = {1, 2, 3, 4};
    auto stat = [&]() { return 0.0; };
    auto shuffle = [&](std::mt19937_64& rng) { std::shuffle(v.begin(), v.end(), rng); };
    const PermutationResult r = permutation_test(stat, shuffle, 1e9, 99, 7);
    REQUIRE(r.p == Approx(1.0 / 100.0).margin(1e-12));
    REQUIRE(r.p > 0.0);
}

TEST_CASE("ridge recovers a known linear relation") {
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 20; ++i) {
        const double x = i * 0.5;
        X.push_back({1.0, x});
        y.push_back(3.0 * x + 2.0);
    }
    std::vector<double> w;
    REQUIRE(solve_ridge(X, y, 1e-10, w));
    REQUIRE(w[0] == Approx(2.0).margin(1e-6));
    REQUIRE(w[1] == Approx(3.0).margin(1e-6));
}

TEST_CASE("a constant column does not produce NaN") {
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 10; ++i) {
        X.push_back({1.0, 1.0});     // perfectly collinear with the intercept
        y.push_back(static_cast<double>(i));
    }
    std::vector<double> w;
    REQUIRE(solve_ridge(X, y, 1e-3, w));
    for (double v : w) REQUIRE(std::isfinite(v));
}

TEST_CASE("a larger ridge penalty shrinks the slope toward zero") {
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    for (int i = 0; i < 20; ++i) {
        const double x = i * 0.5 - 5.0;
        X.push_back({x});
        y.push_back(3.0 * x);
    }
    std::vector<double> small, large;
    REQUIRE(solve_ridge(X, y, 1e-8, small));
    REQUIRE(solve_ridge(X, y, 1e4, large));
    REQUIRE(std::abs(large[0]) < std::abs(small[0]));
}

TEST_CASE("grouped CV refuses to score a single group") {
    // With one session there is nothing to hold out. A number here would be
    // measuring memorisation, so NaN is the only honest return.
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    std::vector<std::string> g;
    for (int i = 0; i < 8; ++i) {
        X.push_back({1.0, static_cast<double>(i)});
        y.push_back(static_cast<double>(i));
        g.push_back("s1");
    }
    REQUIRE(std::isnan(grouped_cv_r2(X, y, g)));
}

TEST_CASE("grouped CV scores a real cross-group relation well") {
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    std::vector<std::string> g;
    for (int s = 0; s < 4; ++s) {
        for (int i = 0; i < 8; ++i) {
            const double x = i * 0.5;
            X.push_back({1.0, x});
            y.push_back(2.0 * x + 1.0);
            g.push_back("s" + std::to_string(s));
        }
    }
    REQUIRE(grouped_cv_r2(X, y, g) > 0.95);
}

TEST_CASE("a group-offset-only dataset scores near zero under grouped CV") {
    // Each group has its own constant offset and no shared signal. Row-wise CV
    // would score high by memorising the offsets from rows of the same session;
    // grouped CV must not, and this is the whole reason validation is grouped.
    std::vector<std::vector<double>> X;
    std::vector<double> y;
    std::vector<std::string> g;
    std::mt19937_64 rng(3);
    std::normal_distribution<double> noise(0.0, 0.05);

    for (int s = 0; s < 5; ++s) {
        const double offset = 10.0 * s;
        for (int i = 0; i < 10; ++i) {
            const double x = i * 0.3;
            X.push_back({1.0, x});
            y.push_back(offset + noise(rng));
            g.push_back("s" + std::to_string(s));
        }
    }
    REQUIRE(grouped_cv_r2(X, y, g) < 0.2);
}
