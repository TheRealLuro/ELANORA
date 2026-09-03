#include "elanora/models/gp.hpp"

#include <cmath>
#include <limits>
#include <sstream>

namespace elanora::models {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Search grid. Inputs are standardised before they reach here, so these ranges
// are meaningful: a length scale of 1 is one standard deviation of the input.
const double kLengthScales[] = {0.25, 0.5, 1.0, 2.0, 4.0};
const double kSignalVars[]   = {0.25, 1.0, 4.0};
const double kNoiseVars[]    = {1e-4, 1e-3, 1e-2, 1e-1};

}  // namespace

double GaussianProcess::kernel(const VectorXd& a, const VectorXd& b) const {
    const double d2 = (a - b).squaredNorm();
    return hyper_.signal_var *
           std::exp(-0.5 * d2 / (hyper_.length_scale * hyper_.length_scale));
}

bool GaussianProcess::factorize(const MatrixXd& X, const VectorXd& y, const GpHyper& h,
                                MatrixXd& L_out, VectorXd& alpha_out,
                                double& lml_out) const {
    const Eigen::Index n = X.rows();
    if (n == 0) return false;

    MatrixXd K(n, n);
    const double inv2l2 = 0.5 / (h.length_scale * h.length_scale);
    for (Eigen::Index i = 0; i < n; ++i) {
        for (Eigen::Index j = 0; j <= i; ++j) {
            const double d2 = (X.row(i) - X.row(j)).squaredNorm();
            const double v = h.signal_var * std::exp(-d2 * inv2l2);
            K(i, j) = v;
            K(j, i) = v;
        }
        K(i, i) += h.noise_var;
    }

    const Eigen::LLT<MatrixXd> llt(K);
    if (llt.info() != Eigen::Success) return false;

    L_out = llt.matrixL();
    alpha_out = llt.solve(y);
    if (!alpha_out.allFinite()) return false;

    // log p(y|X) = -0.5 y'alpha - sum(log L_ii) - (n/2) log 2pi
    double log_det = 0.0;
    for (Eigen::Index i = 0; i < n; ++i) log_det += std::log(L_out(i, i));
    lml_out = -0.5 * y.dot(alpha_out) - log_det -
              0.5 * static_cast<double>(n) * std::log(2.0 * kPi);
    return std::isfinite(lml_out);
}

void GaussianProcess::fit_with(const MatrixXd& X, const VectorXd& y, const GpHyper& h) {
    fitted_ = false;
    if (X.rows() == 0 || y.size() != X.rows()) return;

    MatrixXd L;
    VectorXd alpha;
    double lml = 0.0;
    if (!factorize(X, y, h, L, alpha, lml)) return;

    X_ = X;
    y_ = y;
    L_ = std::move(L);
    alpha_ = std::move(alpha);
    hyper_ = h;
    lml_ = lml;
    fitted_ = true;
}

void GaussianProcess::fit(const MatrixXd& X, const VectorXd& y) {
    fitted_ = false;
    if (X.rows() == 0 || y.size() != X.rows()) return;

    GpHyper best;
    double best_lml = -std::numeric_limits<double>::infinity();
    bool found = false;

    for (double l : kLengthScales) {
        for (double sf : kSignalVars) {
            for (double sn : kNoiseVars) {
                const GpHyper h{l, sf, sn};
                MatrixXd L;
                VectorXd alpha;
                double lml = 0.0;
                if (!factorize(X, y, h, L, alpha, lml)) continue;
                if (lml > best_lml) { best_lml = lml; best = h; found = true; }
            }
        }
    }
    // Every candidate failing means the kernel matrix is not positive definite
    // at any setting -- duplicate rows with a tiny noise floor, typically. The
    // largest noise term is the one most likely to succeed.
    if (!found) best = GpHyper{1.0, 1.0, kNoiseVars[3]};
    fit_with(X, y, best);
}

double GaussianProcess::predict(const VectorXd& x, double* variance) const {
    if (!fitted_ || x.size() != X_.cols()) {
        if (variance) *variance = hyper_.signal_var + hyper_.noise_var;
        return 0.0;
    }

    const Eigen::Index n = X_.rows();
    VectorXd k(n);
    for (Eigen::Index i = 0; i < n; ++i) k(i) = kernel(VectorXd(X_.row(i)), x);

    const double mu = k.dot(alpha_);

    if (variance) {
        // v = L \ k, then var = k(x,x) - v'v. Solving against the triangular
        // factor rather than inverting K keeps this numerically sound.
        const VectorXd v = L_.triangularView<Eigen::Lower>().solve(k);
        double var = hyper_.signal_var - v.squaredNorm() + hyper_.noise_var;
        // Rounding can push this very slightly negative right on top of a
        // training point. A negative variance would produce a NaN standard
        // deviation and poison every score that used it.
        if (!(var > 0.0)) var = hyper_.noise_var;
        *variance = var;
    }
    return mu;
}

std::string GaussianProcess::serialize() const {
    std::ostringstream os;
    os.precision(17);
    os << "hyper " << hyper_.length_scale << ' ' << hyper_.signal_var << ' '
       << hyper_.noise_var << "\n";
    os << "shape " << X_.rows() << ' ' << X_.cols() << "\n";
    os << "X";
    for (Eigen::Index i = 0; i < X_.rows(); ++i) {
        for (Eigen::Index j = 0; j < X_.cols(); ++j) os << ' ' << X_(i, j);
    }
    os << "\ny";
    for (Eigen::Index i = 0; i < y_.size(); ++i) os << ' ' << y_(i);
    os << "\n";
    return os.str();
}

bool GaussianProcess::deserialize(const std::string& text) {
    std::istringstream is(text);
    std::string tag;
    GpHyper h;
    if (!(is >> tag) || tag != "hyper") return false;
    if (!(is >> h.length_scale >> h.signal_var >> h.noise_var)) return false;

    Eigen::Index rows = 0, cols = 0;
    if (!(is >> tag) || tag != "shape") return false;
    if (!(is >> rows >> cols) || rows < 0 || cols < 0) return false;

    MatrixXd X(rows, cols);
    if (!(is >> tag) || tag != "X") return false;
    for (Eigen::Index i = 0; i < rows; ++i) {
        for (Eigen::Index j = 0; j < cols; ++j) {
            if (!(is >> X(i, j))) return false;
        }
    }
    VectorXd y(rows);
    if (!(is >> tag) || tag != "y") return false;
    for (Eigen::Index i = 0; i < rows; ++i) {
        if (!(is >> y(i))) return false;
    }

    // Refactorising rather than storing L keeps the file small and means a
    // hand-edited training set stays consistent with its factorisation.
    fit_with(X, y, h);
    return fitted_;
}

}  // namespace elanora::models
