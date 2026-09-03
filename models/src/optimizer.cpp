#include "elanora/models/optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace elanora::models {

using elanora::data::OutcomeEvidence;
using elanora::data::Verdict;

namespace {

// Points swept across the trained range, in log2 space.
constexpr int kSweepPoints = 200;

// The effect-size floor, in units of the control-trial standard deviation. A
// change smaller than twice the noise measured with no rhythmic stimulus is not
// distinguishable from that noise.
constexpr double kEffectFloorSds = 2.0;

const OutcomeEvidence* find_evidence(const std::vector<OutcomeEvidence>& ev,
                                     const std::string& key) {
    for (const OutcomeEvidence& e : ev) {
        if (e.outcome == key) return &e;
    }
    return nullptr;
}

VectorXd brain_input(double log2f, double baseline_logabs) {
    VectorXd x(2);
    x << log2f, baseline_logabs;
    return x;
}

}  // namespace

const char* goal_name(Goal g) {
    switch (g) {
        case Goal::Increase: return "increase";
        case Goal::Decrease: return "decrease";
        case Goal::Target:   return "target";
        default:             return "ignore";
    }
}

BaselineState BaselineState::from_registry(const ModelRegistry& reg) {
    BaselineState b;
    for (int s = 0; s < kSensorCount; ++s) {
        const ModelId id = static_cast<ModelId>(s);
        for (int k = 0; k < kBandCount; ++k) {
            b.brain_logabs[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] =
                reg.output(id, k).baseline_mean;
        }
    }
    b.bpm = reg.output(ModelId::Heart, 0).baseline_mean;
    b.breathing = reg.output(ModelId::Breathing, 0).baseline_mean;
    return b;
}

BaselineState BaselineState::neutral() {
    BaselineState b;
    for (int s = 0; s < kSensorCount; ++s) {
        for (int k = 0; k < kBandCount; ++k) {
            // log of 1 uV^2: a plausible mid-scale resting value that puts the
            // baseline input near the centre of a typical training range.
            b.brain_logabs[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)] = 0.0;
        }
    }
    b.bpm = 60.0;
    b.breathing = 12.0;
    return b;
}

bool DesiredState::any_goal_set() const {
    for (int s = 0; s < kSensorCount; ++s) {
        for (int k = 0; k < kBandCount; ++k) {
            if (brain[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)].goal !=
                Goal::Ignore) {
                return true;
            }
        }
    }
    return heart.goal != Goal::Ignore || breathing.goal != Goal::Ignore;
}

std::string evidence_key(ModelId m, int output_index, const std::string& response) {
    if (m == ModelId::Heart) return "heart_" + response + "_bpm";
    if (m == ModelId::Breathing) return "breath_" + response + "_breathing";
    if (output_index < 0 || output_index >= kBandCount) return "";
    const SensorId sid = static_cast<SensorId>(static_cast<int>(m));
    return std::string(sensor_name(sid)) + "_" + response + "_logabs_" +
           band_name(static_cast<Band>(output_index));
}

Prediction predict(const ModelRegistry& reg, double frequency_hz, const BaselineState& b) {
    Prediction p;
    p.frequency_hz = frequency_hz;
    if (!(frequency_hz > 0.0)) return p;

    // Models consume log2 frequency. A 1 Hz input becomes feature 0.
    const double log2f = std::log2(frequency_hz);

    for (int s = 0; s < kSensorCount; ++s) {
        const ModelId id = static_cast<ModelId>(s);
        for (int k = 0; k < kBandCount; ++k) {
            const auto si = static_cast<std::size_t>(s);
            const auto ki = static_cast<std::size_t>(k);
            double sd = 0.0;
            const TrainedOutput& o = reg.output(id, k);
            p.max_baseline_z = std::max(p.max_baseline_z, o.baseline_z(b.brain_logabs[si][ki]));
            p.d_bands[si][ki] = o.predict(brain_input(log2f, b.brain_logabs[si][ki]), &sd);
            // A standard deviation of zero would let a goal term divide by it
            // later; every model reports at least its own output spread.
            p.d_bands_sd[si][ki] = std::max(sd, 1e-9);
        }
    }

    {
        double sd = 0.0;
        const TrainedOutput& o = reg.output(ModelId::Heart, 0);
        p.max_baseline_z = std::max(p.max_baseline_z, o.baseline_z(b.bpm));
        p.d_bpm = o.predict(brain_input(log2f, b.bpm), &sd);
        p.d_bpm_sd = std::max(sd, 1e-9);
    }
    {
        double sd = 0.0;
        const TrainedOutput& o = reg.output(ModelId::Breathing, 0);
        p.max_baseline_z = std::max(p.max_baseline_z, o.baseline_z(b.breathing));
        p.d_breathing = o.predict(brain_input(log2f, b.breathing), &sd);
        p.d_breathing_sd = std::max(sd, 1e-9);
    }
    return p;
}

namespace {

// One goal's contribution, already normalised by the outcome's own spread.
//
// Dividing by output_sd is what makes the terms commensurable: band powers move
// by about 0.01 and BPM by about 5, so an unnormalised sum would be a heart
// rate optimizer with a rounding error attached.
struct GoalTerm {
    double match = 0.0;
    double uncertainty = 0.0;
    bool   clears_floor = true;
};

GoalTerm score_goal(const BandGoal& g, double predicted, double predicted_sd,
                    double output_sd, double control_sd, bool require_floor) {
    GoalTerm t;
    if (g.goal == Goal::Ignore) return t;

    const double scale = std::max(output_sd, 1e-9);
    const double z = predicted / scale;

    switch (g.goal) {
        case Goal::Increase: t.match = g.weight * z; break;
        case Goal::Decrease: t.match = -g.weight * z; break;
        case Goal::Target:
            // Distance from the wanted value, negated so closer scores higher.
            t.match = -g.weight * std::abs(predicted - g.target) / scale;
            break;
        default: break;
    }

    t.uncertainty = g.weight * predicted_sd / scale;

    if (require_floor && control_sd > 0.0) {
        t.clears_floor = std::abs(predicted) >= kEffectFloorSds * control_sd;
    }
    return t;
}

}  // namespace

OptimizerResult optimize(const ModelRegistry& reg,
                         const std::vector<OutcomeEvidence>& evidence,
                         const DesiredState& desired, const BaselineState& baseline) {
    OptimizerResult out;

    if (!desired.any_goal_set()) {
        out.reason = "no goal was set, so there is nothing to search for";
        return out;
    }

    const auto [lo_hz, hi_hz] = reg.trained_freq_range();
    if (!(lo_hz > 0.0) || !(hi_hz > lo_hz)) {
        out.reason = "no frequency range has been trained; train the models first";
        return out;
    }

    // ---- gate on evidence -------------------------------------------------
    //
    // Every goal is checked BEFORE any searching. Producing a ranked list and
    // then disclaiming it would still put a number in front of someone.
    const std::string& response = reg.options().response;
    auto check = [&](ModelId id, int k, const BandGoal& g) {
        if (g.goal == Goal::Ignore) return;
        const std::string key = evidence_key(id, k, response);
        const OutcomeEvidence* e = find_evidence(evidence, key);
        if (e == nullptr || e->verdict == Verdict::NoEvidence || !e->tested()) {
            out.unsupported_outcomes.push_back(key);
        }
    };
    for (int s = 0; s < kSensorCount; ++s) {
        for (int k = 0; k < kBandCount; ++k) {
            check(static_cast<ModelId>(s), k,
                  desired.brain[static_cast<std::size_t>(s)][static_cast<std::size_t>(k)]);
        }
    }
    check(ModelId::Heart, 0, desired.heart);
    check(ModelId::Breathing, 0, desired.breathing);

    if (!out.unsupported_outcomes.empty()) {
        out.reason = "no evidence supports a frequency effect on ";
        for (std::size_t i = 0; i < out.unsupported_outcomes.size(); ++i) {
            if (i) out.reason += ", ";
            out.reason += out.unsupported_outcomes[i];
        }
        out.reason += ". Recommendations on these outcomes would not be meaningful.";
        return out;
    }

    // ---- guard the baseline -----------------------------------------------
    //
    // Checked before the sweep, because every candidate shares the baseline. A
    // baseline far outside the training data makes the GP fall back to its
    // prior mean at every frequency, producing a perfectly flat, perfectly
    // confident-looking curve of zeros.
    {
        const Prediction probe = predict(reg, std::exp2(0.5 * (std::log2(lo_hz) +
                                                              std::log2(hi_hz))), baseline);
        if (!probe.baseline_in_range()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.1f", probe.max_baseline_z);
            out.reason = std::string("the supplied baseline is ") + buf +
                         " standard deviations outside the range these models were "
                         "trained on, so their predictions carry no information about it";
            return out;
        }
    }

    // ---- sweep ------------------------------------------------------------
    //
    // In log2 space, matching how the design was spaced and how the models
    // consume frequency. A linear sweep would spend most of its points above
    // 20 Hz and barely resolve anything below 4.
    const double log_lo = std::log2(lo_hz);
    const double log_hi = std::log2(hi_hz);
    const std::vector<double>& trained = reg.trained_frequencies();

    out.ranked.reserve(kSweepPoints);
    for (int i = 0; i < kSweepPoints; ++i) {
        const double t = static_cast<double>(i) / (kSweepPoints - 1);
        const double log2f = log_lo + t * (log_hi - log_lo);

        Candidate c;
        c.frequency_hz = std::exp2(log2f);
        c.pred = predict(reg, c.frequency_hz, baseline);

        bool floor_ok = true;
        auto accumulate = [&](ModelId id, int k, const BandGoal& g, double pred_v,
                              double pred_sd) {
            const GoalTerm term = score_goal(g, pred_v, pred_sd, reg.output_sd(id, k),
                                             reg.control_sd(id, k),
                                             desired.require_effect_floor);
            c.goal_match += term.match;
            c.uncertainty_cost += term.uncertainty;
            if (g.goal != Goal::Ignore && !term.clears_floor) floor_ok = false;
        };

        for (int s = 0; s < kSensorCount; ++s) {
            const auto si = static_cast<std::size_t>(s);
            for (int k = 0; k < kBandCount; ++k) {
                const auto ki = static_cast<std::size_t>(k);
                accumulate(static_cast<ModelId>(s), k, desired.brain[si][ki],
                           c.pred.d_bands[si][ki], c.pred.d_bands_sd[si][ki]);
            }
        }
        accumulate(ModelId::Heart, 0, desired.heart, c.pred.d_bpm, c.pred.d_bpm_sd);
        accumulate(ModelId::Breathing, 0, desired.breathing, c.pred.d_breathing,
                   c.pred.d_breathing_sd);

        c.clears_effect_floor = floor_ok;
        c.uncertainty_cost *= desired.uncertainty_penalty;

        // Distance in log2 to the nearest frequency actually presented. The
        // sweep is continuous but the evidence is not; a point halfway between
        // two tested frequencies is a interpolation, and this says by how much.
        double nearest = std::numeric_limits<double>::infinity();
        for (double f : trained) {
            if (f <= 0.0) continue;
            nearest = std::min(nearest, std::abs(std::log2(f) - log2f));
        }
        if (!std::isfinite(nearest)) nearest = 0.0;
        c.extrapolation_cost = desired.extrapolation_penalty * nearest;

        c.score = c.goal_match - c.uncertainty_cost - c.extrapolation_cost;
        out.ranked.push_back(std::move(c));
    }

    std::sort(out.ranked.begin(), out.ranked.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    if (out.ranked.empty()) {
        out.reason = "the search produced no candidates";
        return out;
    }

    const Candidate& best = out.ranked.front();

    if (best.score < desired.min_score) {
        // A flat score curve is the case this whole design exists to handle.
        // Forcing a pick from it would be inventing a recommendation.
        out.reason = "no frequency scored above the minimum; the predicted responses "
                     "across the trained range are too similar to choose between";
        return out;
    }

    if (desired.require_effect_floor && !best.clears_effect_floor) {
        out.reason = "the best predicted change is smaller than the variation seen on "
                     "control trials, so it cannot be distinguished from noise";
        return out;
    }

    out.found = true;
    return out;
}

}  // namespace elanora::models
