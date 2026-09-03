#pragma once
//
// Grouped cross-validation (Task 26) and the model ladder (Task 27).
//
// Validation is grouped, never row-wise. Trials from one session share
// electrode placement, gel contact, and the subject's state that day. A random
// row split puts some of those trials in train and some in test, so the model
// is scored partly on how well it memorised a session it has already seen --
// which inflates R2 into a number that means nothing about a new person.

#include <Eigen/Dense>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace elanora::models {

using Eigen::MatrixXd;
using Eigen::VectorXd;

struct CvResult {
    double rmse = 0.0;
    double mae  = 0.0;
    double r2   = 0.0;

    // R2 of always predicting the training mean. Reported alongside r2 so that
    // "better than guessing" is something the reader can see rather than
    // assume -- a positive R2 against a badly chosen baseline still means
    // nothing.
    double r2_mean_baseline = 0.0;

    int n       = 0;
    int n_folds = 0;
    std::string scheme;

    bool beats_baseline() const { return r2 > r2_mean_baseline; }
};

// Thrown when validation cannot run at all, rather than returning a number that
// would be silently meaningless.
class ValidationError : public std::runtime_error {
public:
    explicit ValidationError(const std::string& what) : std::runtime_error(what) {}
};

// Fits on the training rows and returns predictions for the test rows.
using FitPredict = std::function<VectorXd(const MatrixXd& X_train, const VectorXd& y_train,
                                          const MatrixXd& X_test)>;

// Leave-one-group-out. `groups` is session_id for session-level validation and
// subject_id for subject-level. Throws ValidationError with fewer than two
// distinct groups.
CvResult grouped_cv(const MatrixXd& X, const VectorXd& y,
                    const std::vector<std::string>& groups,
                    const FitPredict& fit_predict,
                    const std::string& scheme = "leave-one-group-out");

// ---------------------------------------------------------------------------
// The ladder
// ---------------------------------------------------------------------------

enum class ModelKind { MeanBaseline, Ridge, PolyRidge, Gp };

const char* model_kind_name(ModelKind k);
bool parse_model_kind(const std::string& s, ModelKind& out);

struct LadderResult {
    ModelKind chosen = ModelKind::MeanBaseline;
    std::map<ModelKind, CvResult> all;
    std::string note;

    const CvResult& best() const { return all.at(chosen); }
};

// Fits all four candidates under grouped CV and picks the best.
//
// A more complex model must beat a simpler one by `margin` R2 to be chosen.
// Without that margin complexity wins on noise: a GP will always fit at least
// as well in-sample, and on small data its CV advantage over a straight line
// is regularly within the fold-to-fold scatter.
LadderResult select_model(const MatrixXd& X, const VectorXd& y,
                          const std::vector<std::string>& groups,
                          double margin = 0.02);

// The fit-predict function for one rung, usable on its own for final training.
FitPredict make_fit_predict(ModelKind kind);

}  // namespace elanora::models
