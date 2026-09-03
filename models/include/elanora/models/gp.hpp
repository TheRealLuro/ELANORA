#pragma once
//
// Gaussian process regression with an RBF kernel (Task 25).
//
// The GP earns its place here for one reason: it reports how uncertain it is.
// The optimizer has to prefer a confidently mediocre frequency over an
// enticing one it has never seen data near, and a point estimate cannot express
// that difference.
//
// Its known limit, which the optimizer is built around: predictive variance
// SATURATES far from the data. It rises as you leave the training range and
// then flattens at the prior variance. So variance alone cannot stop a
// recommendation at 200 Hz -- a hard clamp to the trained range is still
// required. Task 25's test suite pins that behaviour so it cannot be forgotten.

#include <Eigen/Dense>
#include <string>

namespace elanora::models {

using Eigen::MatrixXd;
using Eigen::VectorXd;

struct GpHyper {
    double length_scale = 1.0;
    double signal_var   = 1.0;
    double noise_var    = 1e-2;
};

class GaussianProcess {
public:
    // Grid-searches the hyperparameters by log marginal likelihood. A gradient
    // optimizer would be faster and far less robust: at the sample sizes here
    // the likelihood surface is bumpy enough that a local method regularly
    // lands somewhere useless.
    void fit(const MatrixXd& X, const VectorXd& y);

    // Fits with the hyperparameters fixed, skipping the search.
    void fit_with(const MatrixXd& X, const VectorXd& y, const GpHyper& h);

    // `variance`, when non-null, receives the predictive variance -- always
    // positive, and including the noise term so it is comparable with the
    // spread of the observations rather than only with the latent function.
    double predict(const VectorXd& x, double* variance = nullptr) const;

    double log_marginal_likelihood() const { return lml_; }
    GpHyper hyper() const { return hyper_; }
    bool fitted() const { return fitted_; }
    Eigen::Index n_train() const { return X_.rows(); }
    Eigen::Index n_features() const { return X_.cols(); }

    std::string serialize() const;
    bool deserialize(const std::string& text);

private:
    double kernel(const VectorXd& a, const VectorXd& b) const;
    bool factorize(const MatrixXd& X, const VectorXd& y, const GpHyper& h,
                   MatrixXd& L_out, VectorXd& alpha_out, double& lml_out) const;

    MatrixXd X_;
    VectorXd y_;
    MatrixXd L_;       // Cholesky factor of K + noise*I
    VectorXd alpha_;   // (K + noise*I)^-1 y
    GpHyper  hyper_;
    double   lml_ = 0.0;
    bool     fitted_ = false;
};

}  // namespace elanora::models
