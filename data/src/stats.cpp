#include "elanora/data/stats.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace elanora::data {

namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

// Modified Lentz continued fraction for the incomplete beta.
double beta_cf(double a, double b, double x) {
    constexpr int kMaxIter = 300;
    constexpr double kEps = 3.0e-14;
    constexpr double kTiny = 1.0e-300;

    const double qab = a + b;
    const double qap = a + 1.0;
    const double qam = a - 1.0;

    double c = 1.0;
    double d = 1.0 - qab * x / qap;
    if (std::abs(d) < kTiny) d = kTiny;
    d = 1.0 / d;
    double h = d;

    for (int m = 1; m <= kMaxIter; ++m) {
        const double m_d = static_cast<double>(m);
        const double m2 = 2.0 * m_d;

        double aa = m_d * (b - m_d) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::abs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        h *= d * c;

        aa = -(a + m_d) * (qab + m_d) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        if (std::abs(d) < kTiny) d = kTiny;
        c = 1.0 + aa / c;
        if (std::abs(c) < kTiny) c = kTiny;
        d = 1.0 / d;
        const double del = d * c;
        h *= del;

        if (std::abs(del - 1.0) < kEps) break;
    }
    return h;
}

}  // namespace

bool is_degenerate(std::span<const double> x) {
    return x.size() < 2 || sd(x) < kMinMeaningfulSd;
}

double mean(std::span<const double> x) {
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (double v : x) s += v;
    return s / static_cast<double>(x.size());
}

double sd(std::span<const double> x) {
    if (x.size() < 2) return 0.0;
    const double m = mean(x);
    double s = 0.0;
    for (double v : x) s += (v - m) * (v - m);
    return std::sqrt(s / static_cast<double>(x.size() - 1));
}

double incomplete_beta(double a, double b, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    if (!(a > 0.0) || !(b > 0.0)) return kNan;

    const double lbeta = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b);
    const double front = std::exp(lbeta + a * std::log(x) + b * std::log(1.0 - x));

    // The continued fraction converges quickly only on one side of the mean;
    // the symmetry relation covers the other.
    if (x < (a + 1.0) / (a + b + 2.0)) {
        return front * beta_cf(a, b, x) / a;
    }
    return 1.0 - front * beta_cf(b, a, 1.0 - x) / b;
}

double t_test_p(double t, double df) {
    if (!(df > 0.0)) return 1.0;
    if (!std::isfinite(t)) return 1.0;
    const double x = df / (df + t * t);
    const double p = incomplete_beta(0.5 * df, 0.5, x);
    return std::clamp(p, 0.0, 1.0);
}

WelchT welch_t_test(std::span<const double> a, std::span<const double> b) {
    WelchT r;
    r.n_a = static_cast<int>(a.size());
    r.n_b = static_cast<int>(b.size());
    if (a.size() < 2 || b.size() < 2) return r;

    r.mean_a = mean(a);
    r.mean_b = mean(b);

    const double va = sd(a) * sd(a);
    const double vb = sd(b) * sd(b);
    const double na = static_cast<double>(a.size());
    const double nb = static_cast<double>(b.size());

    // Both groups constant to the dataset's precision: there is nothing to
    // test, whatever their means happen to be.
    if (is_degenerate(a) && is_degenerate(b)) return r;

    const double se2 = va / na + vb / nb;
    if (!(se2 > 0.0)) {
        // Two constant groups. Identical means are not evidence of anything;
        // different means with zero variance is a degenerate fixture, not a
        // finding, so p stays at 1 either way.
        return r;
    }

    r.t = (r.mean_a - r.mean_b) / std::sqrt(se2);

    // Welch-Satterthwaite. Using n_a + n_b - 2 instead would assume equal
    // variances and overstate the degrees of freedom whenever they differ.
    const double num = se2 * se2;
    const double den = (va / na) * (va / na) / (na - 1.0) +
                       (vb / nb) * (vb / nb) / (nb - 1.0);
    r.df = (den > 0.0) ? num / den : 0.0;
    r.p = t_test_p(r.t, r.df);
    return r;
}

double cohens_d(std::span<const double> a, std::span<const double> b) {
    if (a.size() < 2 || b.size() < 2) return 0.0;
    const double na = static_cast<double>(a.size());
    const double nb = static_cast<double>(b.size());
    const double va = sd(a) * sd(a);
    const double vb = sd(b) * sd(b);
    const double pooled = ((na - 1.0) * va + (nb - 1.0) * vb) / (na + nb - 2.0);
    if (!(pooled > 0.0)) return 0.0;
    return (mean(a) - mean(b)) / std::sqrt(pooled);
}

std::vector<double> benjamini_hochberg(const std::vector<double>& p_raw) {
    const std::size_t n = p_raw.size();
    std::vector<double> out(n, 1.0);
    if (n == 0) return out;

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](std::size_t i, std::size_t j) { return p_raw[i] < p_raw[j]; });

    // Walk from the largest p down, carrying the running minimum. This is what
    // makes the corrected values monotone: a small p can never be corrected to
    // something larger than the correction of a larger p.
    double running_min = 1.0;
    for (std::size_t k = n; k-- > 0;) {
        const std::size_t idx = order[k];
        const double scaled = p_raw[idx] * static_cast<double>(n) /
                              static_cast<double>(k + 1);
        running_min = std::min(running_min, scaled);
        out[idx] = std::clamp(running_min, 0.0, 1.0);
    }
    return out;
}

PermutationResult permutation_test(const std::function<double()>& statistic,
                                   const std::function<void(std::mt19937_64&)>& shuffle,
                                   double observed, int n_perm, uint64_t seed) {
    PermutationResult r;
    r.n_perm = std::max(0, n_perm);
    if (r.n_perm == 0 || !statistic || !shuffle) return r;

    std::mt19937_64 rng(seed);
    std::vector<double> null_dist;
    null_dist.reserve(static_cast<std::size_t>(r.n_perm));

    int at_least = 0;
    for (int i = 0; i < r.n_perm; ++i) {
        shuffle(rng);
        const double s = statistic();
        null_dist.push_back(s);
        if (s >= observed) ++at_least;
    }

    // The observed value is counted in both numerator and denominator, so the
    // smallest reportable p is 1/(n+1). A p of exactly 0 from a finite
    // permutation set claims more precision than the procedure has.
    r.p = static_cast<double>(at_least + 1) / static_cast<double>(r.n_perm + 1);
    r.null_mean = mean(null_dist);
    r.null_sd   = sd(null_dist);
    return r;
}

// ---------------------------------------------------------------------------
// Least squares
// ---------------------------------------------------------------------------

bool solve_ridge(const std::vector<std::vector<double>>& X, const std::vector<double>& y,
                 double lambda, std::vector<double>& w_out) {
    const std::size_t n = X.size();
    if (n == 0 || y.size() != n) return false;
    const std::size_t p = X[0].size();
    if (p == 0) return false;

    // Normal equations. p is at most a handful of columns here, so the
    // conditioning objections to X'X do not bite, and the ridge term covers
    // the degenerate cases that remain.
    std::vector<std::vector<double>> A(p, std::vector<double>(p + 1, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        if (X[i].size() != p) return false;
        for (std::size_t a = 0; a < p; ++a) {
            for (std::size_t b = 0; b < p; ++b) A[a][b] += X[i][a] * X[i][b];
            A[a][p] += X[i][a] * y[i];
        }
    }
    for (std::size_t a = 0; a < p; ++a) A[a][a] += lambda;

    // Gauss-Jordan with partial pivoting.
    for (std::size_t col = 0; col < p; ++col) {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < p; ++r) {
            if (std::abs(A[r][col]) > std::abs(A[pivot][col])) pivot = r;
        }
        if (std::abs(A[pivot][col]) < 1e-12) return false;
        std::swap(A[col], A[pivot]);

        const double d = A[col][col];
        for (std::size_t c = col; c <= p; ++c) A[col][c] /= d;
        for (std::size_t r = 0; r < p; ++r) {
            if (r == col) continue;
            const double f = A[r][col];
            if (f == 0.0) continue;
            for (std::size_t c = col; c <= p; ++c) A[r][c] -= f * A[col][c];
        }
    }

    w_out.resize(p);
    for (std::size_t a = 0; a < p; ++a) w_out[a] = A[a][p];
    return true;
}

double grouped_cv_r2(const std::vector<std::vector<double>>& X,
                     const std::vector<double>& y,
                     const std::vector<std::string>& groups,
                     double lambda) {
    const std::size_t n = X.size();
    if (n < 4 || y.size() != n || groups.size() != n) return kNan;

    std::vector<std::string> distinct;
    for (const std::string& g : groups) {
        if (std::find(distinct.begin(), distinct.end(), g) == distinct.end()) {
            distinct.push_back(g);
        }
    }
    // With one group there is nothing to hold out. Refusing is the only honest
    // answer: any number produced here would be measuring memorisation.
    if (distinct.size() < 2) return kNan;

    std::vector<double> pred(n, 0.0);
    std::vector<bool> have(n, false);

    for (const std::string& held : distinct) {
        std::vector<std::vector<double>> Xtr;
        std::vector<double> ytr;
        for (std::size_t i = 0; i < n; ++i) {
            if (groups[i] == held) continue;
            Xtr.push_back(X[i]);
            ytr.push_back(y[i]);
        }
        if (Xtr.size() < 2) continue;

        std::vector<double> w;
        if (!solve_ridge(Xtr, ytr, lambda, w)) continue;

        for (std::size_t i = 0; i < n; ++i) {
            if (groups[i] != held) continue;
            double acc = 0.0;
            for (std::size_t a = 0; a < w.size() && a < X[i].size(); ++a) acc += w[a] * X[i][a];
            pred[i] = acc;
            have[i] = true;
        }
    }

    double ss_res = 0.0, ss_tot = 0.0;
    std::vector<double> actual;
    for (std::size_t i = 0; i < n; ++i) {
        if (have[i]) actual.push_back(y[i]);
    }
    if (actual.size() < 2) return kNan;
    const double ybar = mean(actual);

    for (std::size_t i = 0; i < n; ++i) {
        if (!have[i]) continue;
        ss_res += (y[i] - pred[i]) * (y[i] - pred[i]);
        ss_tot += (y[i] - ybar) * (y[i] - ybar);
    }
    // Guarded against a constant target, not merely against an exactly zero
    // sum of squares: round-off leaves the latter denormal-positive.
    if (!(ss_tot > 0.0)) return kNan;
    if (is_degenerate(actual)) return kNan;
    return 1.0 - ss_res / ss_tot;
}

}  // namespace elanora::data
