#pragma once
//
// Statistics primitives for the evidence gate (Task 21).
//
// Nothing here is novel; the point is that it is present and tested. The
// alternative -- eyeballing effect sizes -- is how a project like this
// convinces itself of an effect that is not there.

#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace elanora::data {

// Below this standard deviation a quantity is constant as far as the dataset
// can tell. Every value passes through fmt6, so real variation is at least
// 1e-6; anything smaller is round-off, not signal.
//
// This floor is load-bearing. A target that is exactly constant leaves a sum of
// squares that is denormal-positive rather than zero, so a bare "> 0" guard
// passes and R2 comes out around -1e6 -- a number that looks like a
// catastrophic model rather than an absent one.
inline constexpr double kMinMeaningfulSd = 1e-9;

// True when x carries no variation worth testing.
bool is_degenerate(std::span<const double> x);

double mean(std::span<const double> x);
double sd(std::span<const double> x);          // sample standard deviation, n-1

// Regularised incomplete beta. Exposed because it is the piece most likely to
// be wrong, and a wrong t-distribution tail silently shifts every p-value.
double incomplete_beta(double a, double b, double x);

// Two-sided Student's t survival function: P(|T| > |t|) with `df` degrees of
// freedom.
double t_test_p(double t, double df);

struct WelchT {
    double t  = 0.0;
    double df = 0.0;
    double p  = 1.0;   // two-sided
    double mean_a = 0.0;
    double mean_b = 0.0;
    int    n_a = 0;
    int    n_b = 0;

    double difference() const { return mean_a - mean_b; }
};

// Welch's unequal-variance t-test. Student's pooled test assumes the two groups
// share a variance, which stimulus and control trials have no reason to do.
WelchT welch_t_test(std::span<const double> a, std::span<const double> b);

// Benjamini-Hochberg FDR correction.
//
// There are 22 outcomes here. At alpha = 0.05 roughly one of them is expected
// to look significant by chance alone, so an uncorrected gate would pass on
// pure noise -- which is worse than having no gate, because it carries the
// authority of a test.
//
// Returns corrected values in the input order, each clamped to 1 and monotone
// with respect to the sorted raw values.
std::vector<double> benjamini_hochberg(const std::vector<double>& p_raw);

// Cohen's d with a pooled standard deviation. Reported alongside p because a
// p-value says only that an effect is detectable, never that it is large.
double cohens_d(std::span<const double> a, std::span<const double> b);

// Generic permutation test.
//
// `statistic` recomputes the test statistic from whatever state `shuffle` has
// permuted. `p` is the fraction of permutations reaching the observed value,
// with the observed value itself counted -- so the smallest achievable p is
// 1/(n_perm+1) rather than 0. Reporting p = 0 from a finite permutation set
// would overstate the evidence.
struct PermutationResult {
    double p         = 1.0;
    double null_mean = 0.0;
    double null_sd   = 0.0;
    int    n_perm    = 0;
};

PermutationResult permutation_test(const std::function<double()>& statistic,
                                   const std::function<void(std::mt19937_64&)>& shuffle,
                                   double observed, int n_perm, uint64_t seed);

// ---------------------------------------------------------------------------
// Small-p least squares, used by the evidence gate's own model fits
// ---------------------------------------------------------------------------
//
// This duplicates a little of models/ridge.hpp on purpose. The evidence gate
// lives in data/ because it is data analysis, and models/ depends on data/ --
// so data/ cannot reach into models/ without inverting the dependency graph.
// A handful of lines of normal-equations solve is a cheaper price than that
// inversion.

// Solves (X'X + lambda*I) w = X'y for a small number of columns. Returns false
// if the system is singular even after regularisation.
bool solve_ridge(const std::vector<std::vector<double>>& X, const std::vector<double>& y,
                 double lambda, std::vector<double>& w_out);

// Leave-one-group-out R^2.
//
// Grouped, never row-wise. Trials from one session share electrode placement
// and baseline state, so a random row split leaks that shared state into the
// test fold and inflates R2 into meaninglessness.
//
// Returns the R2 against the training mean baseline. NaN when there are fewer
// than two distinct groups -- with one session, grouped validation cannot run
// at all and a number would be a lie.
double grouped_cv_r2(const std::vector<std::vector<double>>& X,
                     const std::vector<double>& y,
                     const std::vector<std::string>& groups,
                     double lambda = 1e-6);

}  // namespace elanora::data
