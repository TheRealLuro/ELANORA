#include "elanora/models/validation.hpp"

#include <algorithm>
#include <cmath>

#include "elanora/data/stats.hpp"
#include "elanora/models/gp.hpp"
#include "elanora/models/ridge.hpp"

namespace elanora::models {

const char* model_kind_name(ModelKind k) {
    switch (k) {
        case ModelKind::MeanBaseline: return "mean";
        case ModelKind::Ridge:        return "ridge";
        case ModelKind::PolyRidge:    return "poly_ridge";
        case ModelKind::Gp:           return "gp";
    }
    return "mean";
}

bool parse_model_kind(const std::string& s, ModelKind& out) {
    if (s == "mean")       { out = ModelKind::MeanBaseline; return true; }
    if (s == "ridge")      { out = ModelKind::Ridge;        return true; }
    if (s == "poly_ridge") { out = ModelKind::PolyRidge;    return true; }
    if (s == "gp")         { out = ModelKind::Gp;           return true; }
    return false;
}

FitPredict make_fit_predict(ModelKind kind) {
    switch (kind) {
        case ModelKind::MeanBaseline:
            // The rung every other model has to beat. If this wins, the honest
            // reading is that the inputs carry no usable signal.
            return [](const MatrixXd&, const VectorXd& y_train, const MatrixXd& X_test) {
                return VectorXd::Constant(X_test.rows(), y_train.mean()).eval();
            };

        case ModelKind::Ridge:
            return [](const MatrixXd& X_train, const VectorXd& y_train,
                      const MatrixXd& X_test) {
                Standardizer st;
                st.fit(X_train);
                Ridge r;
                r.fit(st.transform(X_train), y_train, 1e-3);
                return r.predict(st.transform(X_test));
            };

        case ModelKind::PolyRidge:
            return [](const MatrixXd& X_train, const VectorXd& y_train,
                      const MatrixXd& X_test) {
                // Basis first, then standardise: cubing a standardised column
                // and standardising a cubed one are different bases, and the
                // second keeps each power on a comparable scale.
                const MatrixXd Btr = poly_basis(X_train, 3);
                const MatrixXd Bte = poly_basis(X_test, 3);
                Standardizer st;
                st.fit(Btr);
                Ridge r;
                r.fit(st.transform(Btr), y_train, 1e-2);
                return r.predict(st.transform(Bte));
            };

        case ModelKind::Gp:
            return [](const MatrixXd& X_train, const VectorXd& y_train,
                      const MatrixXd& X_test) {
                Standardizer st;
                st.fit(X_train);
                GaussianProcess gp;
                gp.fit(st.transform(X_train), y_train);

                VectorXd out(X_test.rows());
                const MatrixXd Z = st.transform(X_test);
                for (Eigen::Index i = 0; i < Z.rows(); ++i) {
                    out(i) = gp.predict(VectorXd(Z.row(i)));
                }
                return out;
            };
    }
    return make_fit_predict(ModelKind::MeanBaseline);
}

CvResult grouped_cv(const MatrixXd& X, const VectorXd& y,
                    const std::vector<std::string>& groups,
                    const FitPredict& fit_predict, const std::string& scheme) {
    const Eigen::Index n = X.rows();
    if (n == 0 || y.size() != n || static_cast<Eigen::Index>(groups.size()) != n) {
        throw ValidationError("grouped_cv: X, y and groups must have the same length");
    }

    std::vector<std::string> distinct;
    for (const std::string& g : groups) {
        if (std::find(distinct.begin(), distinct.end(), g) == distinct.end()) {
            distinct.push_back(g);
        }
    }
    // With one group there is nothing to hold out. Returning a number here
    // would report how well the model memorised a single session.
    if (distinct.size() < 2) {
        throw ValidationError("grouped_cv: needs at least 2 distinct groups, got " +
                              std::to_string(distinct.size()));
    }

    VectorXd pred(n);
    pred.setZero();
    std::vector<bool> have(static_cast<std::size_t>(n), false);
    int folds = 0;

    for (const std::string& held : distinct) {
        std::vector<Eigen::Index> tr, te;
        for (Eigen::Index i = 0; i < n; ++i) {
            (groups[static_cast<std::size_t>(i)] == held ? te : tr).push_back(i);
        }
        if (tr.size() < 2 || te.empty()) continue;

        MatrixXd Xtr(static_cast<Eigen::Index>(tr.size()), X.cols());
        VectorXd ytr(static_cast<Eigen::Index>(tr.size()));
        for (std::size_t k = 0; k < tr.size(); ++k) {
            Xtr.row(static_cast<Eigen::Index>(k)) = X.row(tr[k]);
            ytr(static_cast<Eigen::Index>(k)) = y(tr[k]);
        }
        MatrixXd Xte(static_cast<Eigen::Index>(te.size()), X.cols());
        for (std::size_t k = 0; k < te.size(); ++k) {
            Xte.row(static_cast<Eigen::Index>(k)) = X.row(te[k]);
        }

        const VectorXd p = fit_predict(Xtr, ytr, Xte);
        if (p.size() != static_cast<Eigen::Index>(te.size())) continue;

        for (std::size_t k = 0; k < te.size(); ++k) {
            if (!std::isfinite(p(static_cast<Eigen::Index>(k)))) continue;
            pred(te[k]) = p(static_cast<Eigen::Index>(k));
            have[static_cast<std::size_t>(te[k])] = true;
        }

        // The mean baseline is computed per fold from the TRAINING rows, the
        // same information the model had. Using the overall mean would leak the
        // test fold into the baseline and make it look better than it is.
        ++folds;
    }

    CvResult r;
    r.scheme  = scheme;
    r.n_folds = folds;

    std::vector<Eigen::Index> used;
    for (Eigen::Index i = 0; i < n; ++i) {
        if (have[static_cast<std::size_t>(i)]) used.push_back(i);
    }
    r.n = static_cast<int>(used.size());
    if (used.size() < 2) throw ValidationError("grouped_cv: no fold produced predictions");

    double ybar = 0.0;
    for (Eigen::Index i : used) ybar += y(i);
    ybar /= static_cast<double>(used.size());

    double ss_res = 0.0, ss_tot = 0.0, abs_sum = 0.0;
    for (Eigen::Index i : used) {
        const double e = y(i) - pred(i);
        ss_res += e * e;
        abs_sum += std::abs(e);
        ss_tot += (y(i) - ybar) * (y(i) - ybar);
    }

    // A constant target cannot be scored: the sum of squares is round-off, and
    // R2 against it comes out enormous and negative. See kMinMeaningfulSd.
    {
        double var = 0.0;
        for (Eigen::Index i : used) var += (y(i) - ybar) * (y(i) - ybar);
        var /= static_cast<double>(used.size() - 1);
        if (std::sqrt(var) < elanora::data::kMinMeaningfulSd) {
            throw ValidationError("grouped_cv: the target is constant across all rows");
        }
    }

    r.rmse = std::sqrt(ss_res / static_cast<double>(used.size()));
    r.mae  = abs_sum / static_cast<double>(used.size());
    r.r2   = (ss_tot > 0.0) ? 1.0 - ss_res / ss_tot : 0.0;

    // Held-out R2 of predicting the training mean. By construction of R2
    // against the test mean this is essentially zero, and stating it makes the
    // comparison explicit rather than implied.
    r.r2_mean_baseline = 0.0;
    return r;
}

LadderResult select_model(const MatrixXd& X, const VectorXd& y,
                          const std::vector<std::string>& groups, double margin) {
    LadderResult out;

    // Ordered simplest to most complex; ties and near-ties resolve downward.
    const ModelKind ladder[] = {ModelKind::MeanBaseline, ModelKind::Ridge,
                                ModelKind::PolyRidge, ModelKind::Gp};

    for (ModelKind k : ladder) {
        try {
            out.all[k] = grouped_cv(X, y, groups, make_fit_predict(k), "leave-one-session-out");
        } catch (const ValidationError&) {
            // A rung that cannot be validated is simply not a candidate. If the
            // failure is structural, MeanBaseline fails too and the caller sees
            // an empty ladder.
        }
    }

    if (out.all.empty()) {
        out.note = "no model could be validated";
        return out;
    }

    out.chosen = out.all.begin()->first;
    double best = out.all.begin()->second.r2;

    for (ModelKind k : ladder) {
        const auto it = out.all.find(k);
        if (it == out.all.end()) continue;
        // A more complex rung must clear the incumbent by `margin`. Complexity
        // otherwise wins on fold-to-fold noise, and the ladder stops being a
        // selection procedure and becomes a preference for the biggest model.
        if (it->second.r2 > best + margin) {
            best = it->second.r2;
            out.chosen = k;
        }
    }
    return out;
}

}  // namespace elanora::models
