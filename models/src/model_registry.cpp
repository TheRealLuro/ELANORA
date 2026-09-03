#include "elanora/models/model_registry.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "elanora/data/table.hpp"

namespace elanora::models {

namespace fs = std::filesystem;
using elanora::data::Table;

namespace {

const std::string kNoReason;

double sd_of(const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    double m = 0.0;
    for (double x : v) m += x;
    m /= static_cast<double>(v.size());
    double s = 0.0;
    for (double x : v) s += (x - m) * (x - m);
    return std::sqrt(s / static_cast<double>(v.size() - 1));
}

// One outcome's training rows, gathered from a wide ML table.
struct Rows {
    std::vector<std::vector<double>> X;   // [log2_freq, baseline]
    std::vector<double> y;
    std::vector<std::string> session;
    std::vector<std::string> subject;
    std::vector<double> control_y;
    std::vector<double> freqs;
};

Rows gather(const Table& t, const std::string& ycol, const std::string& bcol,
            const std::string& qcol, double min_q) {
    Rows r;
    for (std::size_t i = 0; i < t.rows(); ++i) {
        if (t.num(i, qcol, 0.0) < min_q) continue;
        const double y = t.num(i, ycol, std::nan(""));
        if (!std::isfinite(y)) continue;

        if (t.get(i, "condition") != "stim") {
            r.control_y.push_back(y);
            continue;
        }
        const double lf = t.num(i, "log2_freq", std::nan(""));
        if (!std::isfinite(lf)) continue;

        r.X.push_back({lf, t.num(i, bcol, 0.0)});
        r.y.push_back(y);
        r.session.push_back(t.get(i, "session_id"));
        r.subject.push_back(t.get(i, "subject_id"));
        r.freqs.push_back(t.num(i, "frequency_hz", 0.0));
    }
    return r;
}

MatrixXd to_matrix(const std::vector<std::vector<double>>& X) {
    if (X.empty()) return MatrixXd(0, 0);
    MatrixXd M(static_cast<Eigen::Index>(X.size()), static_cast<Eigen::Index>(X[0].size()));
    for (std::size_t i = 0; i < X.size(); ++i) {
        for (std::size_t j = 0; j < X[i].size(); ++j) {
            M(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = X[i][j];
        }
    }
    return M;
}

VectorXd to_vector(const std::vector<double>& y) {
    VectorXd v(static_cast<Eigen::Index>(y.size()));
    for (std::size_t i = 0; i < y.size(); ++i) v(static_cast<Eigen::Index>(i)) = y[i];
    return v;
}

int distinct_count(const std::vector<std::string>& v) {
    return static_cast<int>(std::set<std::string>(v.begin(), v.end()).size());
}

// Fits the chosen rung on all the data, after the ladder has decided which one.
void fit_final(TrainedOutput& out, const MatrixXd& X, const VectorXd& y) {
    out.mean_fallback = y.mean();
    out.standardizer.fit(X);
    const MatrixXd Z = out.standardizer.transform(X);

    switch (out.kind) {
        case ModelKind::Ridge:
            out.ridge.fit(Z, y, 1e-3);
            break;
        case ModelKind::PolyRidge: {
            const MatrixXd B = poly_basis(X, 3);
            out.standardizer.fit(B);
            out.ridge.fit(out.standardizer.transform(B), y, 1e-2);
            break;
        }
        case ModelKind::Gp:
            out.gp.fit(Z, y);
            break;
        case ModelKind::MeanBaseline:
            break;
    }
    out.trained = true;
}

}  // namespace

const char* model_id_name(ModelId m) {
    switch (m) {
        case ModelId::BrainS1:   return "brain_s1";
        case ModelId::BrainS2:   return "brain_s2";
        case ModelId::BrainS3:   return "brain_s3";
        case ModelId::BrainS4:   return "brain_s4";
        case ModelId::Heart:     return "heart";
        case ModelId::Breathing: return "breathing";
    }
    return "unknown";
}

bool parse_model_id(const std::string& s, ModelId& out) {
    for (int i = 0; i < kModelCount; ++i) {
        const ModelId m = static_cast<ModelId>(i);
        if (s == model_id_name(m)) { out = m; return true; }
    }
    return false;
}

int outputs_for(ModelId m) {
    return (m == ModelId::Heart || m == ModelId::Breathing) ? 1 : kBandCount;
}

std::string output_name(ModelId m, int output_index) {
    if (m == ModelId::Heart) return "d_bpm";
    if (m == ModelId::Breathing) return "d_breathing";
    if (output_index < 0 || output_index >= kBandCount) return "";
    return std::string("d_logabs_") + band_name(static_cast<Band>(output_index));
}

double TrainedOutput::baseline_z(double baseline) const {
    if (!(baseline_sd > 0.0)) return 0.0;
    return std::abs(baseline - baseline_mean) / baseline_sd;
}

double TrainedOutput::predict(const VectorXd& x, double* sd) const {
    if (!trained) {
        if (sd) *sd = output_sd;
        return 0.0;
    }

    switch (kind) {
        case ModelKind::Gp: {
            const VectorXd z = standardizer.transform_row(x);
            double var = 0.0;
            const double mu = gp.predict(z, &var);
            if (sd) *sd = std::sqrt(std::max(var, 0.0));
            return mu;
        }
        case ModelKind::Ridge: {
            const VectorXd z = standardizer.transform_row(x);
            // A ridge fit has no predictive distribution, so its held-out RMSE
            // stands in for one. That is the honest number: it is how wrong the
            // model was on sessions it had not seen.
            if (sd) *sd = (cv_session.n > 0) ? cv_session.rmse : output_sd;
            return ridge.predict(z);
        }
        case ModelKind::PolyRidge: {
            MatrixXd row(1, x.size());
            row.row(0) = x.transpose();
            const MatrixXd B = poly_basis(row, 3);
            const VectorXd z = standardizer.transform_row(VectorXd(B.row(0)));
            if (sd) *sd = (cv_session.n > 0) ? cv_session.rmse : output_sd;
            return ridge.predict(z);
        }
        case ModelKind::MeanBaseline:
        default:
            // The model found no relation. Its uncertainty is the full spread
            // of the data, which is exactly what the optimizer should see.
            if (sd) *sd = output_sd;
            return mean_fallback;
    }
}

void ModelRegistry::train_all(const fs::path& datasets, const TrainOptions& opt) {
    opt_ = opt;
    freqs_.clear();
    for (int i = 0; i < kModelCount; ++i) {
        states_[static_cast<std::size_t>(i)] = ModelState{};
        control_sd_[static_cast<std::size_t>(i)].clear();
    }

    const Table brain  = Table::read_or_empty(datasets / "ml" / "brain_dataset.csv");
    const Table heart  = Table::read_or_empty(datasets / "ml" / "heart_dataset.csv");
    const Table breath = Table::read_or_empty(datasets / "ml" / "breath_dataset.csv");

    std::set<double> freq_set;

    auto train_one = [&](ModelId id, const Table& table, const std::string& sensor_filter,
                         const std::string& ycol, const std::string& bcol,
                         const std::string& qcol, double min_q, int output_index) {
        ModelState& st = state(id);
        if (st.outputs.empty()) {
            st.outputs.resize(static_cast<std::size_t>(outputs_for(id)));
            control_sd_[static_cast<std::size_t>(id)].assign(
                static_cast<std::size_t>(outputs_for(id)), 0.0);
        }
        TrainedOutput& out = st.outputs[static_cast<std::size_t>(output_index)];

        Table filtered = table;
        if (!sensor_filter.empty()) {
            Table sub(table.header());
            for (std::size_t i = 0; i < table.rows(); ++i) {
                if (table.get(i, "sensor") == sensor_filter) sub.add_row(table.row(i));
            }
            filtered = std::move(sub);
        }

        const Rows r = gather(filtered, ycol, bcol, qcol, min_q);
        out.n_samples = static_cast<int>(r.y.size());
        st.n_samples = std::max(st.n_samples, out.n_samples);
        out.output_sd = std::max(sd_of(r.y), 1e-9);

        // The spread of the BASELINE input, kept so the caller can tell whether
        // a supplied baseline is a value these models have ever seen. A query
        // far outside it makes an RBF kernel underflow to zero against every
        // training point, and the GP then returns its prior mean -- which reads
        // as a confident "no change" rather than as "no information".
        {
            std::vector<double> b;
            b.reserve(r.X.size());
            for (const std::vector<double>& row : r.X) {
                if (row.size() > 1) b.push_back(row[1]);
            }
            double m = 0.0;
            for (double v : b) m += v;
            out.baseline_mean = b.empty() ? 0.0 : m / static_cast<double>(b.size());
            out.baseline_sd = std::max(sd_of(b), 1e-9);
        }
        control_sd_[static_cast<std::size_t>(id)][static_cast<std::size_t>(output_index)] =
            sd_of(r.control_y);

        for (double f : r.freqs) {
            if (f > 0.0) freq_set.insert(f);
        }

        if (r.y.size() < 6) {
            st.reason = "fewer than 6 usable stimulus trials";
            return;
        }
        if (distinct_count(r.session) < 2) {
            // One session cannot be validated against another, so the ladder
            // has nothing to choose between. Training anyway would produce a
            // model whose only honest description is "unvalidated".
            st.reason = "only one session; grouped validation cannot run";
            return;
        }

        const MatrixXd X = to_matrix(r.X);
        const VectorXd y = to_vector(r.y);

        const LadderResult ladder = select_model(X, y, r.session, opt_.ladder_margin);
        if (ladder.all.empty()) {
            st.reason = ladder.note.empty() ? "model selection failed" : ladder.note;
            return;
        }
        out.kind = ladder.chosen;
        out.cv_session = ladder.best();

        // Subject-level validation, when there is more than one subject. The
        // gap between session-level and subject-level R2 is the real measure of
        // whether any of this generalises to a new person.
        if (distinct_count(r.subject) >= 2) {
            try {
                out.cv_subject = grouped_cv(X, y, r.subject, make_fit_predict(out.kind),
                                            "leave-one-subject-out");
                out.has_subject_cv = true;
            } catch (const ValidationError&) {
                out.has_subject_cv = false;
            }
        }

        fit_final(out, X, y);
        st.reason.clear();
    };

    for (int s = 0; s < kSensorCount; ++s) {
        const ModelId id = static_cast<ModelId>(s);
        const std::string sensor = sensor_name(static_cast<SensorId>(s));
        for (int b = 0; b < kBandCount; ++b) {
            const std::string band = band_name(static_cast<Band>(b));
            train_one(id, brain, sensor, opt_.response + "_logabs_" + band,
                      "baseline_logabs_" + band, "quality_eeg", opt_.min_quality_eeg, b);
        }
    }

    train_one(ModelId::Heart, heart, "", opt_.response + "_bpm", "baseline_bpm",
              "quality_heart", opt_.min_quality_heart, 0);
    train_one(ModelId::Breathing, breath, "", opt_.response + "_breathing",
              "baseline_breathing", "confidence_breath", opt_.min_confidence_breath, 0);

    freqs_.assign(freq_set.begin(), freq_set.end());
}

bool ModelRegistry::trained(ModelId m) const {
    const ModelState& st = state(m);
    if (st.outputs.empty()) return false;
    for (const TrainedOutput& o : st.outputs) {
        if (!o.trained) return false;
    }
    return true;
}

bool ModelRegistry::any_trained() const {
    for (int i = 0; i < kModelCount; ++i) {
        if (trained(static_cast<ModelId>(i))) return true;
    }
    return false;
}

int ModelRegistry::n_samples(ModelId m) const { return state(m).n_samples; }

const std::string& ModelRegistry::untrained_reason(ModelId m) const {
    return trained(m) ? kNoReason : state(m).reason;
}

const TrainedOutput& ModelRegistry::output(ModelId m, int output_index) const {
    static const TrainedOutput kEmpty;
    const ModelState& st = state(m);
    if (output_index < 0 || static_cast<std::size_t>(output_index) >= st.outputs.size()) {
        return kEmpty;
    }
    return st.outputs[static_cast<std::size_t>(output_index)];
}

CvResult ModelRegistry::cv(ModelId m, int output_index) const {
    return output(m, output_index).cv_session;
}

ModelKind ModelRegistry::kind(ModelId m, int output_index) const {
    return output(m, output_index).kind;
}

double ModelRegistry::output_sd(ModelId m, int output_index) const {
    return output(m, output_index).output_sd;
}

double ModelRegistry::control_sd(ModelId m, int output_index) const {
    const auto& v = control_sd_[static_cast<std::size_t>(m)];
    if (output_index < 0 || static_cast<std::size_t>(output_index) >= v.size()) return 0.0;
    return v[static_cast<std::size_t>(output_index)];
}

std::pair<double, double> ModelRegistry::trained_freq_range() const {
    if (freqs_.empty()) return {0.0, 0.0};
    return {freqs_.front(), freqs_.back()};
}

// ---------------------------------------------------------------------------
// Persistence -- plain text, so a trained model can be inspected and diffed
// ---------------------------------------------------------------------------

bool ModelRegistry::save(const fs::path& dir, std::string& err) const {
    err.clear();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { err = "cannot create " + dir.string(); return false; }

    std::ofstream out(dir / "registry.txt", std::ios::binary);
    if (!out) { err = "cannot write registry.txt"; return false; }
    out.precision(17);

    out << "elanora_models 1\n";
    out << "response " << opt_.response << "\n";
    out << "frequencies " << freqs_.size();
    for (double f : freqs_) out << ' ' << f;
    out << "\n";

    for (int i = 0; i < kModelCount; ++i) {
        const ModelId id = static_cast<ModelId>(i);
        const ModelState& st = state(id);
        out << "model " << model_id_name(id) << ' ' << st.outputs.size() << ' '
            << st.n_samples << ' ' << (st.reason.empty() ? "-" : st.reason) << "\n";

        for (std::size_t k = 0; k < st.outputs.size(); ++k) {
            const TrainedOutput& o = st.outputs[k];
            out << "output " << k << ' ' << model_kind_name(o.kind) << ' '
                << (o.trained ? 1 : 0) << ' ' << o.n_samples << ' ' << o.output_sd << ' '
                << o.mean_fallback << ' ' << control_sd(id, static_cast<int>(k)) << "\n";
            out << "baseline " << o.baseline_mean << ' ' << o.baseline_sd << "\n";
            out << "cv " << o.cv_session.rmse << ' ' << o.cv_session.mae << ' '
                << o.cv_session.r2 << ' ' << o.cv_session.n << ' ' << o.cv_session.n_folds << "\n";
            out << "cvsubj " << (o.has_subject_cv ? 1 : 0) << ' ' << o.cv_subject.r2 << ' '
                << o.cv_subject.rmse << ' ' << o.cv_subject.n << "\n";
            out << "standardizer\n" << o.standardizer.serialize() << "end\n";
            out << "ridge\n" << o.ridge.serialize() << "end\n";
            out << "gp\n" << o.gp.serialize() << "end\n";
        }
    }
    return static_cast<bool>(out);
}

namespace {

// Reads lines until a lone "end", returning the block.
std::string read_block(std::istream& is) {
    std::string block, line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "end") break;
        block += line;
        block += '\n';
    }
    return block;
}

}  // namespace

bool ModelRegistry::load(const fs::path& dir, std::string& err) {
    err.clear();
    std::ifstream in(dir / "registry.txt", std::ios::binary);
    if (!in) { err = "no registry.txt in " + dir.string(); return false; }

    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "elanora_models") {
        err = "not an ELANORA model file";
        return false;
    }
    if (version != 1) { err = "unsupported model file version"; return false; }

    for (int i = 0; i < kModelCount; ++i) {
        states_[static_cast<std::size_t>(i)] = ModelState{};
        control_sd_[static_cast<std::size_t>(i)].clear();
    }
    freqs_.clear();

    while (in >> tag) {
        if (tag == "response") {
            in >> opt_.response;
        } else if (tag == "frequencies") {
            std::size_t n = 0;
            in >> n;
            freqs_.resize(n);
            for (std::size_t k = 0; k < n; ++k) in >> freqs_[k];
        } else if (tag == "model") {
            std::string name;
            std::size_t n_out = 0;
            int n_samp = 0;
            in >> name >> n_out >> n_samp;
            std::string reason;
            std::getline(in, reason);
            if (!reason.empty() && reason.front() == ' ') reason.erase(0, 1);
            if (!reason.empty() && reason.back() == '\r') reason.pop_back();

            ModelId id{};
            if (!parse_model_id(name, id)) { err = "unknown model id: " + name; return false; }

            ModelState& st = state(id);
            st.outputs.assign(n_out, TrainedOutput{});
            st.n_samples = n_samp;
            st.reason = (reason == "-") ? "" : reason;
            control_sd_[static_cast<std::size_t>(id)].assign(n_out, 0.0);

            for (std::size_t k = 0; k < n_out; ++k) {
                std::size_t idx = 0;
                std::string kind_name;
                int is_trained = 0;
                double csd = 0.0;
                TrainedOutput o;
                in >> tag >> idx >> kind_name >> is_trained >> o.n_samples >> o.output_sd
                   >> o.mean_fallback >> csd;
                if (tag != "output") { err = "malformed model file"; return false; }
                if (!parse_model_kind(kind_name, o.kind)) { err = "unknown model kind"; return false; }
                o.trained = (is_trained != 0);
                control_sd_[static_cast<std::size_t>(id)][idx] = csd;

                in >> tag >> o.baseline_mean >> o.baseline_sd;
                if (tag != "baseline") { err = "malformed model file"; return false; }

                in >> tag >> o.cv_session.rmse >> o.cv_session.mae >> o.cv_session.r2
                   >> o.cv_session.n >> o.cv_session.n_folds;
                int has_subj = 0;
                in >> tag >> has_subj >> o.cv_subject.r2 >> o.cv_subject.rmse >> o.cv_subject.n;
                o.has_subject_cv = (has_subj != 0);

                std::string line;
                std::getline(in, line);   // finish the cvsubj line

                std::getline(in, line);   // "standardizer"
                o.standardizer.deserialize(read_block(in));
                std::getline(in, line);   // "ridge"
                o.ridge.deserialize(read_block(in));
                std::getline(in, line);   // "gp"
                o.gp.deserialize(read_block(in));

                if (idx < st.outputs.size()) st.outputs[idx] = std::move(o);
            }
        }
    }
    return true;
}

}  // namespace elanora::models
