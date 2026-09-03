#include "elanora/data/evidence.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include "elanora/csv.hpp"
#include "elanora/data/stats.hpp"
#include "elanora/data/table.hpp"

namespace elanora::data {

namespace fs = std::filesystem;

const char* verdict_name(Verdict v) {
    switch (v) {
        case Verdict::Supported: return "supported";
        case Verdict::Weak:      return "weak";
        default:                 return "no_evidence";
    }
}

namespace {

// One outcome's rows, already filtered for quality and split by condition.
struct OutcomeData {
    std::vector<double> stim;        // response values, stimulus condition
    std::vector<double> control;     // response values, both control conditions
    std::vector<double> log2f;       // stimulus rows only
    std::vector<double> baseline;    // stimulus rows only
    std::vector<double> y;           // stimulus rows only, same order as log2f
    std::vector<std::string> groups; // session id per stimulus row
};

bool usable(double v) { return std::isfinite(v); }

// Design matrix [1, log2f, log2f^2, baseline].
//
// A quadratic in log-frequency, not a straight line: the hypothesis under test
// is that some frequency region does something, which is a peak, and a linear
// term cannot represent a peak at all. Baseline is included because the same
// frequency plausibly does different things to a relaxed and an alert person.
std::vector<std::vector<double>> design(const std::vector<double>& log2f,
                                        const std::vector<double>& baseline) {
    std::vector<std::vector<double>> X;
    X.reserve(log2f.size());
    for (std::size_t i = 0; i < log2f.size(); ++i) {
        X.push_back({1.0, log2f[i], log2f[i] * log2f[i], baseline[i]});
    }
    return X;
}

void evaluate(OutcomeEvidence& e, const OutcomeData& d, const EvidenceOptions& opt) {
    e.n_stim    = static_cast<int>(d.stim.size());
    e.n_control = static_cast<int>(d.control.size());

    if (d.stim.size() < 4) {
        e.insufficient = "fewer than 4 usable stimulus trials";
        return;
    }
    if (is_degenerate(d.stim)) {
        // Constant to the precision it was stored at. Distinct from "we tested
        // and found nothing": there was nothing here that could vary.
        e.insufficient = "the response is constant across every trial";
        e.mean_stim = mean(d.stim);
        return;
    }

    // ---- stimulus versus pooled controls ---------------------------------
    if (d.control.size() >= 2) {
        const WelchT w = welch_t_test(d.stim, d.control);
        e.p_raw        = w.p;
        e.mean_stim    = w.mean_a;
        e.mean_control = w.mean_b;
        e.effect_stim_vs_control = cohens_d(d.stim, d.control);
    } else {
        e.insufficient = "no control trials to compare against";
        e.mean_stim = mean(d.stim);
    }

    // ---- does frequency predict the response? ----------------------------
    std::vector<std::string> distinct;
    for (const std::string& g : d.groups) {
        if (std::find(distinct.begin(), distinct.end(), g) == distinct.end()) {
            distinct.push_back(g);
        }
    }
    if (distinct.size() < 2) {
        // One session cannot be validated against another. Reporting an R2 here
        // would measure how well the model memorised a single session.
        if (e.insufficient.empty()) {
            e.insufficient = "only one session; grouped validation cannot run";
        }
        return;
    }

    const std::vector<std::vector<double>> X = design(d.log2f, d.baseline);
    const double r2 = grouped_cv_r2(X, d.y, d.groups);
    if (!std::isfinite(r2)) {
        if (e.insufficient.empty()) e.insufficient = "model fit failed";
        return;
    }
    e.cv_r2 = r2;

    // Permute the frequency labels WITHIN session. Shuffling across sessions
    // would break the session structure too, and the resulting null would be
    // easy to beat for reasons that have nothing to do with frequency.
    std::vector<double> permuted = d.log2f;
    std::map<std::string, std::vector<std::size_t>> by_group;
    for (std::size_t i = 0; i < d.groups.size(); ++i) by_group[d.groups[i]].push_back(i);

    auto shuffle = [&](std::mt19937_64& rng) {
        for (auto& [g, idx] : by_group) {
            std::vector<double> vals;
            vals.reserve(idx.size());
            for (std::size_t i : idx) vals.push_back(d.log2f[i]);
            std::shuffle(vals.begin(), vals.end(), rng);
            for (std::size_t k = 0; k < idx.size(); ++k) permuted[idx[k]] = vals[k];
        }
    };
    auto statistic = [&]() {
        const double v = grouped_cv_r2(design(permuted, d.baseline), d.y, d.groups);
        return std::isfinite(v) ? v : -1.0;
    };

    const PermutationResult perm =
        permutation_test(statistic, shuffle, r2, opt.n_permutations, opt.seed);
    e.p_frequency     = perm.p;
    e.cv_r2_null_mean = perm.null_mean;
}

Verdict decide(const OutcomeEvidence& e, const EvidenceOptions& opt) {
    if (!e.tested()) return Verdict::NoEvidence;

    // Two independent conditions. Differing from control says the stimulus did
    // something; beating the permuted null says frequency is what did it.
    const bool differs_from_control = e.p_fdr < opt.alpha;
    const bool frequency_predicts   = e.cv_r2 > e.cv_r2_null_mean &&
                                      e.p_frequency < opt.alpha;

    if (differs_from_control && frequency_predicts) return Verdict::Supported;
    if (differs_from_control || frequency_predicts) return Verdict::Weak;
    return Verdict::NoEvidence;
}

}  // namespace

std::vector<OutcomeEvidence> analyze(const fs::path& root, const EvidenceOptions& opt) {
    std::vector<OutcomeEvidence> out;

    const Table brain  = Table::read_or_empty(root / "ml" / "brain_dataset.csv");
    const Table heart  = Table::read_or_empty(root / "ml" / "heart_dataset.csv");
    const Table breath = Table::read_or_empty(root / "ml" / "breath_dataset.csv");

    // ---- 20 brain outcomes ------------------------------------------------
    for (int s = 0; s < kSensorCount; ++s) {
        const SensorId sid = static_cast<SensorId>(s);
        for (int b = 0; b < kBandCount; ++b) {
            const Band band = static_cast<Band>(b);

            OutcomeEvidence e;
            e.family = "brain";
            e.sensor = sensor_name(sid);
            e.outcome = std::string(sensor_name(sid)) + "_" + opt.response + "_logabs_" +
                        band_name(band);

            const std::string ycol = opt.response + "_logabs_" + band_name(band);
            const std::string bcol = std::string("baseline_logabs_") + band_name(band);

            OutcomeData d;
            for (std::size_t i = 0; i < brain.rows(); ++i) {
                if (brain.get(i, "sensor") != sensor_name(sid)) continue;
                if (brain.num(i, "quality_eeg", 0.0) < opt.min_quality_eeg) continue;

                const double y = brain.num(i, ycol, std::nan(""));
                if (!usable(y)) continue;

                const std::string cond = brain.get(i, "condition");
                if (cond == "stim") {
                    const double lf = brain.num(i, "log2_freq", std::nan(""));
                    if (!usable(lf)) continue;
                    d.stim.push_back(y);
                    d.y.push_back(y);
                    d.log2f.push_back(lf);
                    d.baseline.push_back(brain.num(i, bcol, 0.0));
                    d.groups.push_back(brain.get(i, "session_id"));
                } else {
                    d.control.push_back(y);
                }
            }
            evaluate(e, d, opt);
            out.push_back(std::move(e));
        }
    }

    // ---- heart and breathing ---------------------------------------------
    auto scalar_outcome = [&](const Table& t, const char* family, const char* ycol,
                              const char* bcol, const char* qcol, double min_q,
                              const std::string& name) {
        OutcomeEvidence e;
        e.family  = family;
        e.outcome = name;

        OutcomeData d;
        for (std::size_t i = 0; i < t.rows(); ++i) {
            if (t.num(i, qcol, 0.0) < min_q) continue;
            const double y = t.num(i, ycol, std::nan(""));
            if (!usable(y)) continue;

            if (t.get(i, "condition") == "stim") {
                const double lf = t.num(i, "log2_freq", std::nan(""));
                if (!usable(lf)) continue;
                d.stim.push_back(y);
                d.y.push_back(y);
                d.log2f.push_back(lf);
                d.baseline.push_back(t.num(i, bcol, 0.0));
                d.groups.push_back(t.get(i, "session_id"));
            } else {
                d.control.push_back(y);
            }
        }
        evaluate(e, d, opt);
        out.push_back(std::move(e));
    };

    scalar_outcome(heart, "heart", (opt.response + "_bpm").c_str(), "baseline_bpm",
                   "quality_heart", opt.min_quality_heart, "heart_" + opt.response + "_bpm");
    scalar_outcome(breath, "breath", (opt.response + "_breathing").c_str(),
                   "baseline_breathing", "confidence_breath", opt.min_confidence_breath,
                   "breath_" + opt.response + "_breathing");

    // ---- FDR across all 22 ------------------------------------------------
    //
    // Applied over every outcome that was actually tested. Including untestable
    // outcomes at p = 1 would inflate the denominator and make the correction
    // more lenient than it should be.
    std::vector<double> raw;
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!out[i].tested()) continue;
        raw.push_back(out[i].p_raw);
        idx.push_back(i);
    }
    const std::vector<double> corrected = benjamini_hochberg(raw);
    for (std::size_t k = 0; k < idx.size(); ++k) out[idx[k]].p_fdr = corrected[k];

    for (OutcomeEvidence& e : out) e.verdict = decide(e, opt);
    return out;
}

void write_evidence(const fs::path& root, const std::vector<OutcomeEvidence>& ev) {
    Table t({"outcome", "family", "sensor", "n_stim", "n_control",
             "effect_stim_vs_control", "mean_stim", "mean_control",
             "p_raw", "p_fdr", "cv_r2", "cv_r2_null_mean", "p_frequency",
             "verdict", "insufficient"});
    for (const OutcomeEvidence& e : ev) {
        t.add_row({e.outcome, e.family, e.sensor, std::to_string(e.n_stim),
                   std::to_string(e.n_control), fmt6(e.effect_stim_vs_control),
                   fmt6(e.mean_stim), fmt6(e.mean_control), fmt6(e.p_raw),
                   fmt6(e.p_fdr), fmt6(e.cv_r2), fmt6(e.cv_r2_null_mean),
                   fmt6(e.p_frequency), verdict_name(e.verdict), e.insufficient});
    }
    t.write(root / "analysis" / "evidence.csv");
}

std::vector<OutcomeEvidence> read_evidence(const fs::path& root) {
    std::vector<OutcomeEvidence> out;
    const Table t = Table::read_or_empty(root / "analysis" / "evidence.csv");
    for (std::size_t i = 0; i < t.rows(); ++i) {
        OutcomeEvidence e;
        e.outcome = t.get(i, "outcome");
        e.family  = t.get(i, "family");
        e.sensor  = t.get(i, "sensor");
        e.n_stim    = static_cast<int>(t.num(i, "n_stim"));
        e.n_control = static_cast<int>(t.num(i, "n_control"));
        e.effect_stim_vs_control = t.num(i, "effect_stim_vs_control");
        e.mean_stim    = t.num(i, "mean_stim");
        e.mean_control = t.num(i, "mean_control");
        e.p_raw = t.num(i, "p_raw", 1.0);
        e.p_fdr = t.num(i, "p_fdr", 1.0);
        e.cv_r2 = t.num(i, "cv_r2");
        e.cv_r2_null_mean = t.num(i, "cv_r2_null_mean");
        e.p_frequency = t.num(i, "p_frequency", 1.0);
        e.insufficient = t.get(i, "insufficient");

        const std::string v = t.get(i, "verdict");
        e.verdict = (v == "supported") ? Verdict::Supported
                  : (v == "weak")      ? Verdict::Weak
                                       : Verdict::NoEvidence;
        out.push_back(std::move(e));
    }
    return out;
}

EvidenceSummary summarize(const std::vector<OutcomeEvidence>& ev) {
    EvidenceSummary s;
    for (const OutcomeEvidence& e : ev) {
        if (!e.tested()) { ++s.untested; continue; }
        switch (e.verdict) {
            case Verdict::Supported: ++s.supported; break;
            case Verdict::Weak:      ++s.weak;      break;
            default:                 ++s.none;      break;
        }
    }
    return s;
}

}  // namespace elanora::data
