#pragma once
//
// Standardisation and ridge regression (Task 24).
//
// Both are deliberately plain. The interesting modelling decisions in this
// project are which model to use (Task 27) and whether the data supports any
// model at all (Milestone 4) -- not the regressor's internals.

#include <Eigen/Dense>
#include <string>
#include <vector>

namespace elanora::models {

using Eigen::MatrixXd;
using Eigen::VectorXd;

// Centres and scales each column to zero mean and unit variance.
//
// Required before the GP: its kernel measures Euclidean distance across all
// input dimensions at once, so a feature in units of 5 (BPM) and one in units
// of 0.01 (log band power) would give the first all the influence and the
// second none. A single length scale only makes sense on standardised inputs.
class Standardizer {
public:
    void fit(const MatrixXd& X);
    MatrixXd transform(const MatrixXd& X) const;
    VectorXd transform_row(const VectorXd& x) const;

    const VectorXd& mean() const { return mean_; }
    const VectorXd& scale() const { return scale_; }
    bool fitted() const { return mean_.size() > 0; }

    std::string serialize() const;
    bool deserialize(const std::string& text);

private:
    VectorXd mean_;
    VectorXd scale_;   // 1.0 substituted where a column has no variance
};

class Ridge {
public:
    // Centres X and y, solves (X'X + lambda*I) w = X'y, then restores the
    // intercept from the means. Centring first means lambda never penalises the
    // intercept, which would otherwise bias every prediction toward zero.
    void fit(const MatrixXd& X, const VectorXd& y, double lambda);

    double predict(const VectorXd& x) const;
    VectorXd predict(const MatrixXd& X) const;

    const VectorXd& weights() const { return w_; }
    double intercept() const { return b_; }
    bool fitted() const { return fitted_; }

    std::string serialize() const;
    bool deserialize(const std::string& text);

private:
    VectorXd w_;
    double   b_ = 0.0;
    bool     fitted_ = false;
};

// Degree-3 polynomial basis over the first column only.
//
// The first column is always log2_freq by construction of the model registry.
// Expanding every column would produce a basis larger than the dataset at the
// sample sizes this project works with.
MatrixXd poly_basis(const MatrixXd& X, int degree = 3);

}  // namespace elanora::models
