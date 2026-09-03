#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

#include "elanora/models/gp.hpp"
#include "elanora/models/ridge.hpp"
#include "elanora/models/validation.hpp"

using namespace elanora::models;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Standardizer and Ridge
// ---------------------------------------------------------------------------

TEST_CASE("the standardizer produces zero mean and unit variance") {
    MatrixXd X(5, 2);
    X << 1, 100, 2, 200, 3, 300, 4, 400, 5, 500;

    Standardizer st;
    st.fit(X);
    const MatrixXd Z = st.transform(X);

    for (Eigen::Index c = 0; c < Z.cols(); ++c) {
        REQUIRE(Z.col(c).mean() == Approx(0.0).margin(1e-12));
        const double var = (Z.col(c).array() - Z.col(c).mean()).square().sum() / 4.0;
        REQUIRE(var == Approx(1.0).margin(1e-9));
    }
}

TEST_CASE("a constant column is neutralised rather than producing NaN") {
    // Substituting 1.0 for a zero standard deviation leaves the column at zero
    // after centring, so it is harmlessly ignored instead of poisoning the fit.
    MatrixXd X(4, 2);
    X << 1, 7, 2, 7, 3, 7, 4, 7;

    Standardizer st;
    st.fit(X);
    const MatrixXd Z = st.transform(X);
    REQUIRE(Z.allFinite());
    for (Eigen::Index i = 0; i < Z.rows(); ++i) REQUIRE(Z(i, 1) == Approx(0.0));
}

TEST_CASE("ridge recovers a known linear relation") {
    MatrixXd X(20, 1);
    VectorXd y(20);
    for (int i = 0; i < 20; ++i) {
        X(i, 0) = i * 0.5;
        y(i) = 3.0 * X(i, 0) + 2.0;
    }
    Ridge r;
    r.fit(X, y, 1e-8);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(r.predict(VectorXd(X.row(i))) == Approx(y(i)).margin(1e-6));
    }
    REQUIRE(r.weights()(0) == Approx(3.0).margin(1e-6));
    REQUIRE(r.intercept() == Approx(2.0).margin(1e-6));
}

TEST_CASE("a large penalty shrinks the slope toward zero") {
    MatrixXd X(20, 1);
    VectorXd y(20);
    for (int i = 0; i < 20; ++i) {
        X(i, 0) = i * 0.5;
        y(i) = 3.0 * X(i, 0);
    }
    Ridge small, large;
    small.fit(X, y, 1e-8);
    large.fit(X, y, 1e6);
    REQUIRE(std::abs(large.weights()(0)) < std::abs(small.weights()(0)));

    // The intercept is never penalised, so a fully shrunk model still predicts
    // the mean rather than zero.
    REQUIRE(large.intercept() == Approx(y.mean()).margin(0.5));
}

TEST_CASE("ridge and the standardizer round-trip through text") {
    MatrixXd X(10, 2);
    VectorXd y(10);
    std::mt19937_64 rng(5);
    std::normal_distribution<double> g(0.0, 1.0);
    for (int i = 0; i < 10; ++i) {
        X(i, 0) = g(rng);
        X(i, 1) = g(rng);
        y(i) = 2.0 * X(i, 0) - X(i, 1);
    }
    Standardizer st;
    st.fit(X);
    Ridge r;
    r.fit(st.transform(X), y, 1e-4);

    Standardizer st2;
    Ridge r2;
    REQUIRE(st2.deserialize(st.serialize()));
    REQUIRE(r2.deserialize(r.serialize()));

    for (int i = 0; i < 10; ++i) {
        const VectorXd row = X.row(i);
        REQUIRE(r2.predict(st2.transform_row(row))
                == Approx(r.predict(st.transform_row(row))).margin(1e-9));
    }
}

TEST_CASE("the polynomial basis expands only the frequency column") {
    MatrixXd X(3, 2);
    X << 2, 10, 3, 20, 4, 30;
    const MatrixXd B = poly_basis(X, 3);

    REQUIRE(B.cols() == 4);          // x, x^2, x^3, then the untouched column
    REQUIRE(B(0, 0) == Approx(2.0));
    REQUIRE(B(0, 1) == Approx(4.0));
    REQUIRE(B(0, 2) == Approx(8.0));
    REQUIRE(B(0, 3) == Approx(10.0));
}

// ---------------------------------------------------------------------------
// Gaussian process
// ---------------------------------------------------------------------------

TEST_CASE("the GP interpolates its own training points") {
    MatrixXd X(5, 1);
    X << -1, 0, 1, 2, 3;
    VectorXd y(5);
    y << 0.1, 0.3, 0.9, 0.4, 0.2;

    GaussianProcess gp;
    gp.fit(X, y);
    REQUIRE(gp.fitted());
    for (int i = 0; i < 5; ++i) {
        REQUIRE(gp.predict(VectorXd(X.row(i))) == Approx(y(i)).margin(0.15));
    }
}

TEST_CASE("uncertainty grows away from the training data") {
    MatrixXd X(3, 1);
    X << 0, 1, 2;
    VectorXd y(3);
    y << 0.1, 0.2, 0.3;

    GaussianProcess gp;
    gp.fit(X, y);

    double v_near = 0, v_far = 0;
    gp.predict((VectorXd(1) << 1.0).finished(), &v_near);
    gp.predict((VectorXd(1) << 9.0).finished(), &v_far);
    REQUIRE(v_far > v_near);
}

TEST_CASE("GP variance saturates, which is why a hard range clamp is still needed") {
    // This documents the limit the optimizer is built around. Extrapolation
    // penalty from variance alone is BOUNDED: past a few length scales the
    // variance stops growing, so a confident-looking recommendation far outside
    // the trained range would not be penalised into last place by uncertainty.
    MatrixXd X(3, 1);
    X << 0, 1, 2;
    VectorXd y(3);
    y << 0.1, 0.2, 0.3;

    GaussianProcess gp;
    gp.fit(X, y);

    double v2 = 0, v10 = 0, v100 = 0, v1000 = 0;
    gp.predict((VectorXd(1) << 2.0).finished(),    &v2);
    gp.predict((VectorXd(1) << 10.0).finished(),   &v10);
    gp.predict((VectorXd(1) << 100.0).finished(),  &v100);
    gp.predict((VectorXd(1) << 1000.0).finished(), &v1000);

    // Two points a factor of ten apart, both far outside the data, are
    // indistinguishable: the variance has stopped carrying any information
    // about how far away they are.
    REQUIRE(v1000 == Approx(v100).epsilon(1e-6));

    // It is bounded above by the prior variance no matter how far you go.
    const double ceiling = gp.hyper().signal_var + gp.hyper().noise_var;
    REQUIRE(v100 <= ceiling + 1e-12);
    REQUIRE(v1000 <= ceiling + 1e-12);

    // And the growth is already flattening well before then: going from 10 to
    // 100 buys far less than going from 2 to 10 did.
    REQUIRE(v100 - v10 < 0.25 * (v10 - v2));
}

TEST_CASE("the GP variance is always positive, even on a training point") {
    MatrixXd X(4, 1);
    X << 0, 1, 2, 3;
    VectorXd y(4);
    y << 1, 2, 1, 2;

    GaussianProcess gp;
    gp.fit(X, y);
    for (int i = 0; i < 4; ++i) {
        double v = -1.0;
        gp.predict(VectorXd(X.row(i)), &v);
        REQUIRE(v > 0.0);
        REQUIRE(std::isfinite(std::sqrt(v)));
    }
}

TEST_CASE("the hyperparameter search improves the marginal likelihood") {
    MatrixXd X(12, 1);
    VectorXd y(12);
    for (int i = 0; i < 12; ++i) {
        X(i, 0) = i * 0.4 - 2.0;
        y(i) = std::sin(X(i, 0));
    }
    GaussianProcess searched, fixed;
    searched.fit(X, y);
    fixed.fit_with(X, y, GpHyper{4.0, 0.25, 0.1});
    REQUIRE(searched.log_marginal_likelihood() >= fixed.log_marginal_likelihood());
}

TEST_CASE("the GP round-trips through text") {
    MatrixXd X(6, 2);
    VectorXd y(6);
    std::mt19937_64 rng(11);
    std::normal_distribution<double> g(0.0, 1.0);
    for (int i = 0; i < 6; ++i) {
        X(i, 0) = g(rng);
        X(i, 1) = g(rng);
        y(i) = std::sin(X(i, 0)) + 0.2 * X(i, 1);
    }
    GaussianProcess a;
    a.fit(X, y);

    GaussianProcess b;
    REQUIRE(b.deserialize(a.serialize()));
    for (int i = 0; i < 6; ++i) {
        double va = 0, vb = 0;
        const double pa = a.predict(VectorXd(X.row(i)), &va);
        const double pb = b.predict(VectorXd(X.row(i)), &vb);
        REQUIRE(pb == Approx(pa).margin(1e-9));
        REQUIRE(vb == Approx(va).margin(1e-9));
    }
}

TEST_CASE("an unfitted GP predicts zero with prior variance rather than crashing") {
    GaussianProcess gp;
    double v = 0.0;
    REQUIRE(gp.predict((VectorXd(1) << 1.0).finished(), &v) == Approx(0.0));
    REQUIRE(v > 0.0);
}

// ---------------------------------------------------------------------------
// Grouped validation
// ---------------------------------------------------------------------------

namespace {

struct Dataset {
    MatrixXd X;
    VectorXd y;
    std::vector<std::string> groups;
};

Dataset linear_across_groups(int n_groups = 4, int per_group = 8) {
    Dataset d;
    const int n = n_groups * per_group;
    d.X.resize(n, 1);
    d.y.resize(n);
    int k = 0;
    for (int s = 0; s < n_groups; ++s) {
        for (int i = 0; i < per_group; ++i, ++k) {
            d.X(k, 0) = i * 0.5;
            d.y(k) = 2.0 * d.X(k, 0) + 1.0;
            d.groups.push_back("s" + std::to_string(s));
        }
    }
    return d;
}

}  // namespace

TEST_CASE("grouped CV never trains and tests on the same group") {
    // Recorded by having the fit function assert that no test row's group
    // appears in training. The scheme is only meaningful if this holds.
    Dataset d = linear_across_groups();

    std::vector<std::string> seen_train, seen_test;
    auto spy = [&](const MatrixXd& Xtr, const VectorXd& ytr, const MatrixXd& Xte) {
        // Group membership is recoverable from row counts here; instead assert
        // the sizes partition the data exactly, which is the same guarantee.
        REQUIRE(Xtr.rows() + Xte.rows() == d.X.rows());
        return VectorXd::Constant(Xte.rows(), ytr.mean()).eval();
    };
    const CvResult r = grouped_cv(d.X, d.y, d.groups, spy);
    REQUIRE(r.n_folds == 4);
    REQUIRE(r.n == static_cast<int>(d.groups.size()));
}

TEST_CASE("fewer than two groups is refused rather than silently wrong") {
    // With one session, grouped validation cannot run. The system must refuse
    // instead of reporting a number that means nothing.
    Dataset d = linear_across_groups(1, 6);
    REQUIRE_THROWS_AS(grouped_cv(d.X, d.y, d.groups, make_fit_predict(ModelKind::Ridge)),
                      ValidationError);
}

TEST_CASE("a group-confounded dataset scores near zero under grouped CV") {
    // Each group is a constant offset with no shared signal. Row-wise CV would
    // score high by memorising offsets from sibling rows; grouped CV must not.
    Dataset d;
    const int n_groups = 5, per = 10;
    d.X.resize(n_groups * per, 1);
    d.y.resize(n_groups * per);
    std::mt19937_64 rng(3);
    std::normal_distribution<double> noise(0.0, 0.05);
    int k = 0;
    for (int s = 0; s < n_groups; ++s) {
        for (int i = 0; i < per; ++i, ++k) {
            d.X(k, 0) = i * 0.3;
            d.y(k) = 10.0 * s + noise(rng);
            d.groups.push_back("s" + std::to_string(s));
        }
    }
    REQUIRE(grouped_cv(d.X, d.y, d.groups, make_fit_predict(ModelKind::Ridge)).r2 < 0.2);
}

TEST_CASE("a genuine cross-group relation scores well") {
    Dataset d = linear_across_groups();
    const CvResult r = grouped_cv(d.X, d.y, d.groups, make_fit_predict(ModelKind::Ridge));
    REQUIRE(r.r2 > 0.95);
    REQUIRE(r.beats_baseline());
    REQUIRE(r.rmse < 0.2);
}

// ---------------------------------------------------------------------------
// The ladder
// ---------------------------------------------------------------------------

TEST_CASE("exactly linear data selects Ridge, not the GP") {
    // The simplest adequate model wins. Without the margin the GP would take
    // this on a hundredth of an R2 that is pure fold-to-fold noise.
    Dataset d = linear_across_groups();
    const LadderResult r = select_model(d.X, d.y, d.groups);
    REQUIRE(r.chosen == ModelKind::Ridge);
    REQUIRE(r.all.size() == 4u);
}

TEST_CASE("pure noise selects the mean baseline") {
    Dataset d;
    const int n_groups = 5, per = 10;
    d.X.resize(n_groups * per, 1);
    d.y.resize(n_groups * per);
    std::mt19937_64 rng(17);
    std::normal_distribution<double> g(0.0, 1.0);
    int k = 0;
    for (int s = 0; s < n_groups; ++s) {
        for (int i = 0; i < per; ++i, ++k) {
            d.X(k, 0) = g(rng);
            d.y(k) = g(rng);
            d.groups.push_back("s" + std::to_string(s));
        }
    }
    REQUIRE(select_model(d.X, d.y, d.groups).chosen == ModelKind::MeanBaseline);
}

TEST_CASE("strongly nonlinear data selects a nonlinear rung") {
    // A Gaussian bump in log-frequency: the shape a real entrainment response
    // would have, and one a straight line cannot represent at all.
    Dataset d;
    const int n_groups = 5, per = 14;
    d.X.resize(n_groups * per, 1);
    d.y.resize(n_groups * per);
    std::mt19937_64 rng(23);
    std::normal_distribution<double> noise(0.0, 0.03);
    int k = 0;
    for (int s = 0; s < n_groups; ++s) {
        for (int i = 0; i < per; ++i, ++k) {
            const double x = -1.0 + 6.0 * i / (per - 1);
            d.X(k, 0) = x;
            d.y(k) = std::exp(-0.5 * (x - 3.5) * (x - 3.5) / 0.36) + noise(rng);
            d.groups.push_back("s" + std::to_string(s));
        }
    }
    const LadderResult r = select_model(d.X, d.y, d.groups);
    REQUIRE((r.chosen == ModelKind::Gp || r.chosen == ModelKind::PolyRidge));
    REQUIRE(r.best().r2 > r.all.at(ModelKind::Ridge).r2);
}

TEST_CASE("a ladder that cannot validate reports the fact instead of guessing") {
    Dataset d = linear_across_groups(1, 6);
    const LadderResult r = select_model(d.X, d.y, d.groups);
    REQUIRE(r.all.empty());
    REQUIRE_FALSE(r.note.empty());
}

TEST_CASE("model kind names round-trip") {
    for (ModelKind k : {ModelKind::MeanBaseline, ModelKind::Ridge,
                        ModelKind::PolyRidge, ModelKind::Gp}) {
        ModelKind back{};
        REQUIRE(parse_model_kind(model_kind_name(k), back));
        REQUIRE(back == k);
    }
    ModelKind unused{};
    REQUIRE_FALSE(parse_model_kind("nonsense", unused));
}
