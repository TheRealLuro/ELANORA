#include "elanora/models/ridge.hpp"

#include <cmath>
#include <sstream>

namespace elanora::models {

namespace {

// Serialisation is plain text on purpose: a trained model that cannot be read
// in a text editor is a model nobody will ever check.
std::string vec_to_text(const VectorXd& v) {
    std::ostringstream os;
    os.precision(17);
    os << v.size();
    for (Eigen::Index i = 0; i < v.size(); ++i) os << ' ' << v(i);
    return os.str();
}

bool text_to_vec(std::istringstream& is, VectorXd& out) {
    Eigen::Index n = 0;
    if (!(is >> n) || n < 0) return false;
    out.resize(n);
    for (Eigen::Index i = 0; i < n; ++i) {
        if (!(is >> out(i))) return false;
    }
    return true;
}

}  // namespace

void Standardizer::fit(const MatrixXd& X) {
    if (X.rows() == 0 || X.cols() == 0) return;

    mean_ = X.colwise().mean();
    scale_.resize(X.cols());

    for (Eigen::Index c = 0; c < X.cols(); ++c) {
        const double var = (X.col(c).array() - mean_(c)).square().sum() /
                           std::max<double>(1.0, static_cast<double>(X.rows() - 1));
        const double s = std::sqrt(var);
        // A constant column carries no information. Substituting 1 leaves it at
        // zero after centring rather than producing inf or NaN, so the column is
        // harmlessly ignored instead of destroying the fit.
        scale_(c) = (s > 1e-12) ? s : 1.0;
    }
}

MatrixXd Standardizer::transform(const MatrixXd& X) const {
    if (!fitted() || X.cols() != mean_.size()) return X;
    MatrixXd out = X;
    for (Eigen::Index c = 0; c < X.cols(); ++c) {
        out.col(c) = (X.col(c).array() - mean_(c)) / scale_(c);
    }
    return out;
}

VectorXd Standardizer::transform_row(const VectorXd& x) const {
    if (!fitted() || x.size() != mean_.size()) return x;
    return (x - mean_).array() / scale_.array();
}

std::string Standardizer::serialize() const {
    return "mean " + vec_to_text(mean_) + "\nscale " + vec_to_text(scale_) + "\n";
}

bool Standardizer::deserialize(const std::string& text) {
    std::istringstream is(text);
    std::string tag;
    if (!(is >> tag) || tag != "mean") return false;
    if (!text_to_vec(is, mean_)) return false;
    if (!(is >> tag) || tag != "scale") return false;
    return text_to_vec(is, scale_);
}

void Ridge::fit(const MatrixXd& X, const VectorXd& y, double lambda) {
    fitted_ = false;
    if (X.rows() == 0 || X.cols() == 0 || y.size() != X.rows()) return;

    const VectorXd xbar = X.colwise().mean();
    const double   ybar = y.mean();

    MatrixXd Xc = X;
    for (Eigen::Index c = 0; c < X.cols(); ++c) Xc.col(c).array() -= xbar(c);
    const VectorXd yc = y.array() - ybar;

    const MatrixXd A = Xc.transpose() * Xc +
                       lambda * MatrixXd::Identity(X.cols(), X.cols());
    w_ = A.ldlt().solve(Xc.transpose() * yc);

    // LDLT can return non-finite values on a badly conditioned system without
    // reporting failure. Silently predicting NaN forever is worse than not
    // fitting at all.
    if (!w_.allFinite()) { w_.setZero(X.cols()); }

    b_ = ybar - xbar.dot(w_);
    fitted_ = std::isfinite(b_);
    if (!fitted_) b_ = 0.0;
}

double Ridge::predict(const VectorXd& x) const {
    if (!fitted_ || x.size() != w_.size()) return b_;
    return b_ + x.dot(w_);
}

VectorXd Ridge::predict(const MatrixXd& X) const {
    VectorXd out(X.rows());
    for (Eigen::Index i = 0; i < X.rows(); ++i) out(i) = predict(VectorXd(X.row(i)));
    return out;
}

std::string Ridge::serialize() const {
    std::ostringstream os;
    os.precision(17);
    os << "intercept " << b_ << "\nweights " << vec_to_text(w_) << "\n";
    return os.str();
}

bool Ridge::deserialize(const std::string& text) {
    std::istringstream is(text);
    std::string tag;
    if (!(is >> tag) || tag != "intercept") return false;
    if (!(is >> b_)) return false;
    if (!(is >> tag) || tag != "weights") return false;
    if (!text_to_vec(is, w_)) return false;
    fitted_ = true;
    return true;
}

MatrixXd poly_basis(const MatrixXd& X, int degree) {
    if (X.cols() == 0 || degree < 1) return X;

    // The first column expanded to `degree` powers, then the remaining columns
    // appended unchanged.
    const Eigen::Index extra = X.cols() - 1;
    MatrixXd out(X.rows(), degree + extra);
    for (int d = 1; d <= degree; ++d) {
        out.col(d - 1) = X.col(0).array().pow(static_cast<double>(d));
    }
    for (Eigen::Index c = 1; c < X.cols(); ++c) out.col(degree + c - 1) = X.col(c);
    return out;
}

}  // namespace elanora::models
