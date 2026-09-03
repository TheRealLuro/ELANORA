// The optimizer's refusal paths matter more than its recommendations.
//
// A system that always returns a frequency is worthless if frequency turns out
// not to predict anything, so most of what follows checks that the three
// guards -- evidence, trained range, effect floor -- actually stop it.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
#include <random>

#include "elanora/csv.hpp"
#include "elanora/data/table.hpp"
#include "elanora/models/model_registry.hpp"
#include "elanora/models/optimizer.hpp"

using namespace elanora;
using namespace elanora::models;
using elanora::data::OutcomeEvidence;
using elanora::data::Table;
using elanora::data::Verdict;
using Catch::Approx;

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("elanora_mdl_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

const std::vector<double> kFreqs = {0.5, 0.71, 1.0, 1.41, 2.0, 2.83, 4.0,
                                    5.66, 8.0, 11.3, 16.0, 22.6, 32.0, 45.0};

struct Spec {
    int    sessions      = 5;
    int    subjects      = 2;
    double peak_hz       = 11.0;   // 0 for a flat response
    double peak_gain     = 0.8;
    double noise_sd      = 0.05;
    double control_noise = 0.05;
    std::vector<double> freqs = kFreqs;
    uint64_t seed = 77;
};

// Writes ml/*.csv directly. The feature layer has its own tests; going through
// raw synthesis here would make each of these cases minutes long.
void write_ml(const fs::path& root, const Spec& sp) {
    std::vector<std::string> bh = {"subject_id", "session_id", "trial_id", "condition",
                                   "frequency_hz", "log2_freq", "sensor", "electrode"};
    for (const char* rep : {"logabs", "clr"}) {
        for (const char* stage : {"baseline", "stimulus", "post", "d", "post_d", "recovery"}) {
            for (int b = 0; b < kBandCount; ++b) {
                bh.push_back(std::string(stage) + "_" + rep + "_" +
                             band_name(static_cast<Band>(b)));
            }
        }
    }
    bh.push_back("quality_eeg");
    Table brain(bh);

    auto scalar_header = [](const char* stem, const char* extra, const char* q) {
        std::vector<std::string> h = {"subject_id", "session_id", "trial_id", "condition",
                                      "frequency_hz", "log2_freq"};
        for (const char* stage : {"baseline", "stimulus", "post", "d", "post_d", "recovery"}) {
            h.push_back(std::string(stage) + "_" + stem);
        }
        h.push_back(std::string("baseline_") + extra);
        h.push_back(std::string("d_") + extra);
        h.push_back(q);
        return h;
    };
    Table heart(scalar_header("bpm", "rmssd", "quality_heart"));
    Table breath(scalar_header("breathing", "breathing_spectral", "confidence_breath"));

    std::mt19937_64 rng(sp.seed);
    std::normal_distribution<double> noise(0.0, sp.noise_sd);
    std::normal_distribution<double> cnoise(0.0, sp.control_noise);

    for (int s = 0; s < sp.sessions; ++s) {
        const std::string session = "SESS" + std::to_string(s);
        const std::string subject = "P" + std::to_string(s % sp.subjects);
        int round = 0;

        auto emit = [&](const std::string& condition, double hz) {
            const std::string trial = session + "_R" + std::to_string(++round);
            const std::string log2f = (hz > 0.0) ? fmt6(std::log2(hz)) : std::string();
            const bool is_stim = (condition == "stim");

            double effect = 0.0;
            if (is_stim && sp.peak_hz > 0.0 && hz > 0.0) {
                const double dz = std::log2(hz) - std::log2(sp.peak_hz);
                effect = sp.peak_gain * std::exp(-0.5 * dz * dz / (0.6 * 0.6));
            }

            for (int sensor = 0; sensor < kSensorCount; ++sensor) {
                const SensorId sid = static_cast<SensorId>(sensor);
                std::vector<std::string> row = {subject, session, trial, condition,
                                                fmt6(hz), log2f, sensor_name(sid),
                                                electrode_name(sid)};
                for (int rep = 0; rep < 2; ++rep) {
                    for (int stage = 0; stage < 6; ++stage) {
                        for (int b = 0; b < kBandCount; ++b) {
                            // Only S1 alpha carries the frequency response.
                            const bool target =
                                (sensor == 0 && b == index_of(Band::Alpha) && rep == 0);
                            double v = is_stim ? noise(rng) : cnoise(rng);
                            if (target && stage == 3) v += effect;
                            if (target && stage == 0) v += 1.0;
                            row.push_back(fmt6(v));
                        }
                    }
                }
                row.push_back(fmt6(0.9));
                brain.add_row(std::move(row));
            }

            std::vector<std::string> h = {subject, session, trial, condition, fmt6(hz), log2f};
            for (int stage = 0; stage < 6; ++stage) {
                h.push_back(fmt6((stage == 0 ? 60.0 : 0.0) + noise(rng)));
            }
            h.push_back(fmt6(30.0));
            h.push_back(fmt6(noise(rng)));
            h.push_back(fmt6(0.9));
            heart.add_row(std::move(h));

            std::vector<std::string> br = {subject, session, trial, condition, fmt6(hz), log2f};
            for (int stage = 0; stage < 6; ++stage) {
                br.push_back(fmt6((stage == 0 ? 12.0 : 0.0) + noise(rng)));
            }
            br.push_back(fmt6(12.0));
            br.push_back(fmt6(noise(rng)));
            br.push_back(fmt6(0.8));
            breath.add_row(std::move(br));
        };

        for (double hz : sp.freqs) emit("stim", hz);
        emit("control_jitter", 10.0);
        emit("control_tone", 0.0);
    }

    brain.write(root / "ml" / "brain_dataset.csv");
    heart.write(root / "ml" / "heart_dataset.csv");
    breath.write(root / "ml" / "breath_dataset.csv");
}

std::vector<OutcomeEvidence> evidence_all(Verdict v, const std::string& response = "d") {
    std::vector<OutcomeEvidence> ev;
    for (int s = 0; s < kSensorCount; ++s) {
        for (int b = 0; b < kBandCount; ++b) {
            OutcomeEvidence e;
            e.outcome = evidence_key(static_cast<ModelId>(s), b, response);
            e.verdict = v;
            e.n_stim = 20;
            ev.push_back(e);
        }
    }
    OutcomeEvidence h;
    h.outcome = evidence_key(ModelId::Heart, 0, response);
    h.verdict = v;
    h.n_stim = 20;
    ev.push_back(h);

    OutcomeEvidence b;
    b.outcome = evidence_key(ModelId::Breathing, 0, response);
    b.verdict = v;
    b.n_stim = 20;
    ev.push_back(b);
    return ev;
}

DesiredState alpha_increase_goal() {
    DesiredState d;
    d.brain[0][index_of(Band::Alpha)] = {Goal::Increase, 0.0, 1.0};
    d.require_effect_floor = false;   // enabled explicitly in the floor tests
    return d;
}

ModelRegistry train(const fs::path& root) {
    ModelRegistry reg;
    reg.train_all(root);
    return reg;
}

}  // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

TEST_CASE("training fills all six models from a multi-session dataset") {
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    for (int i = 0; i < kModelCount; ++i) {
        const ModelId m = static_cast<ModelId>(i);
        INFO("model " << model_id_name(m) << ": " << reg.untrained_reason(m));
        REQUIRE(reg.trained(m));
        REQUIRE(reg.n_samples(m) > 0);
    }
    REQUIRE(reg.any_trained());
}

TEST_CASE("the trained frequency range matches the presented frequencies") {
    TempDir tmp;
    Spec sp;
    sp.freqs = {4.0, 5.66, 8.0, 11.3, 16.0};
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    const auto [lo, hi] = reg.trained_freq_range();
    REQUIRE(lo == Approx(4.0));
    REQUIRE(hi == Approx(16.0));
    REQUIRE(reg.trained_frequencies().size() == 5u);
}

TEST_CASE("a single session leaves the models untrained with a readable reason") {
    TempDir tmp;
    Spec sp;
    sp.sessions = 1;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    REQUIRE_FALSE(reg.trained(ModelId::BrainS1));
    REQUIRE_THAT(reg.untrained_reason(ModelId::BrainS1),
                 Catch::Matchers::ContainsSubstring("one session"));
}

TEST_CASE("the registry round-trips through disk") {
    TempDir tmp, out;
    write_ml(tmp.path, Spec{});
    const ModelRegistry a = train(tmp.path);

    std::string err;
    REQUIRE(a.save(out.path, err));
    REQUIRE(err.empty());

    ModelRegistry b;
    REQUIRE(b.load(out.path, err));
    REQUIRE(err.empty());

    REQUIRE(b.trained(ModelId::BrainS1));
    REQUIRE(b.trained_freq_range() == a.trained_freq_range());

    const BaselineState base = BaselineState::from_registry(a);
    for (double hz : {2.0, 8.0, 11.0, 32.0}) {
        const Prediction pa = predict(a, hz, base);
        const Prediction pb = predict(b, hz, base);
        const auto ai = static_cast<std::size_t>(index_of(Band::Alpha));
        REQUIRE(pb.d_bands[0][ai] == Approx(pa.d_bands[0][ai]).margin(1e-9));
        REQUIRE(pb.d_bpm == Approx(pa.d_bpm).margin(1e-9));
    }
}

TEST_CASE("loading a missing registry reports the problem instead of throwing") {
    TempDir tmp;
    ModelRegistry reg;
    std::string err;
    REQUIRE_FALSE(reg.load(tmp.path, err));
    REQUIRE_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// Forward prediction
// ---------------------------------------------------------------------------

TEST_CASE("a prediction fills all 22 means and 22 positive standard deviations") {
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);
    const Prediction p = predict(reg, 10.0, BaselineState::from_registry(reg));

    for (int s = 0; s < kSensorCount; ++s) {
        for (int b = 0; b < kBandCount; ++b) {
            const auto si = static_cast<std::size_t>(s);
            const auto bi = static_cast<std::size_t>(b);
            REQUIRE(std::isfinite(p.d_bands[si][bi]));
            REQUIRE(p.d_bands_sd[si][bi] > 0.0);
        }
    }
    REQUIRE(std::isfinite(p.d_bpm));
    REQUIRE(p.d_bpm_sd > 0.0);
    REQUIRE(std::isfinite(p.d_breathing));
    REQUIRE(p.d_breathing_sd > 0.0);
}

TEST_CASE("frequency converts to log2 for the models") {
    // 1 Hz must reach the model as feature 0. An off-by-a-transform here would
    // shift every prediction along the frequency axis.
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);
    REQUIRE(predict(reg, 1.0, BaselineState::from_registry(reg)).frequency_hz == Approx(1.0));
    REQUIRE(predict(reg, 0.0, BaselineState::from_registry(reg)).frequency_hz == Approx(0.0));
}

TEST_CASE("the learned response peaks near where the data peaks") {
    TempDir tmp;
    Spec sp;
    sp.peak_hz = 11.0;
    sp.peak_gain = 1.0;
    sp.noise_sd = 0.03;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    const auto ai = static_cast<std::size_t>(index_of(Band::Alpha));
    const double at_peak = predict(reg, 11.0, BaselineState::from_registry(reg)).d_bands[0][ai];
    const double at_edge = predict(reg, 0.5, BaselineState::from_registry(reg)).d_bands[0][ai];
    REQUIRE(at_peak > at_edge);
}

// ---------------------------------------------------------------------------
// The inverse optimizer
// ---------------------------------------------------------------------------

TEST_CASE("the optimizer picks the frequency whose predicted response matches the goal") {
    TempDir tmp;
    Spec sp;
    sp.peak_hz = 11.0;
    sp.peak_gain = 1.0;
    sp.noise_sd = 0.03;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    DesiredState d = alpha_increase_goal();
    d.min_score = 0.0;
    const OptimizerResult r =
        optimize(reg, evidence_all(Verdict::Supported), d, BaselineState::from_registry(reg));

    REQUIRE(r.found);
    REQUIRE_FALSE(r.ranked.empty());
    REQUIRE(r.ranked.front().frequency_hz == Approx(11.0).margin(3.0));
}

TEST_CASE("goals on outcomes with no statistical evidence are refused") {
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    const OptimizerResult r = optimize(reg, evidence_all(Verdict::NoEvidence),
                                       alpha_increase_goal(), BaselineState::from_registry(reg));
    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("no evidence"));
    REQUIRE(r.unsupported_outcomes.size() == 1u);
    REQUIRE(r.unsupported_outcomes[0] == "S1_d_logabs_alpha");
}

TEST_CASE("an outcome absent from the evidence table is treated as unsupported") {
    // Missing is not the same as passing. A goal on an outcome nobody tested
    // must refuse exactly as one that was tested and found nothing.
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    const OptimizerResult r = optimize(reg, {}, alpha_increase_goal(),
                                       BaselineState::from_registry(reg));
    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("no evidence"));
}

TEST_CASE("a flat score curve returns no recommendation") {
    TempDir tmp;
    Spec sp;
    sp.peak_hz = 0.0;      // no frequency effect at all
    sp.noise_sd = 0.05;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    DesiredState d = alpha_increase_goal();
    d.min_score = 0.35;
    const OptimizerResult r =
        optimize(reg, evidence_all(Verdict::Supported), d, BaselineState::from_registry(reg));

    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("no frequency"));
}

TEST_CASE("the search never leaves the trained frequency range") {
    // GP variance saturates, so uncertainty alone cannot keep the sweep inside
    // the data. The clamp is what does it.
    TempDir tmp;
    Spec sp;
    sp.freqs = {4.0, 5.66, 8.0, 11.3, 16.0};
    sp.peak_hz = 8.0;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    DesiredState d = alpha_increase_goal();
    d.min_score = 0.0;
    const OptimizerResult r =
        optimize(reg, evidence_all(Verdict::Supported), d, BaselineState::from_registry(reg));

    REQUIRE_FALSE(r.ranked.empty());
    for (const Candidate& c : r.ranked) {
        REQUIRE(c.frequency_hz >= 4.0 - 1e-9);
        REQUIRE(c.frequency_hz <= 16.0 + 1e-9);
    }
}

TEST_CASE("an untested frequency is penalised for distance from the data") {
    TempDir tmp;
    Spec sp;
    sp.freqs = {4.0, 32.0};    // a wide gap in the middle
    sp.peak_hz = 0.0;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    DesiredState d = alpha_increase_goal();
    d.min_score = -1e9;        // rank everything, refuse nothing
    d.extrapolation_penalty = 1.0;
    const OptimizerResult r =
        optimize(reg, evidence_all(Verdict::Supported), d, BaselineState::from_registry(reg));

    // The midpoint between 4 and 32 Hz is 1.5 octaves from either, and must
    // carry a visibly larger extrapolation cost than a tested frequency.
    const Candidate* mid = nullptr;
    const Candidate* edge = nullptr;
    for (const Candidate& c : r.ranked) {
        if (std::abs(c.frequency_hz - 11.3) < 0.6) mid = &c;
        if (std::abs(c.frequency_hz - 4.0) < 0.1) edge = &c;
    }
    REQUIRE(mid != nullptr);
    REQUIRE(edge != nullptr);
    REQUIRE(mid->extrapolation_cost > edge->extrapolation_cost);
    REQUIRE(edge->extrapolation_cost == Approx(0.0).margin(0.05));
}

TEST_CASE("a change smaller than control noise is refused by the effect floor") {
    TempDir tmp;
    Spec sp;
    sp.peak_hz = 11.0;
    sp.peak_gain = 0.02;     // a real but tiny effect
    sp.control_noise = 0.5;  // control trials vary far more than that
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    DesiredState d = alpha_increase_goal();
    d.require_effect_floor = true;
    d.min_score = -1e9;      // isolate the floor from the score threshold

    const OptimizerResult r =
        optimize(reg, evidence_all(Verdict::Supported), d, BaselineState::from_registry(reg));
    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("control trials"));
}

TEST_CASE("setting no goal is refused rather than answered arbitrarily") {
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    DesiredState empty;
    const OptimizerResult r = optimize(reg, evidence_all(Verdict::Supported), empty,
                                       BaselineState::from_registry(reg));
    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("no goal"));
}

TEST_CASE("an untrained registry refuses instead of recommending") {
    ModelRegistry reg;
    const OptimizerResult r = optimize(reg, evidence_all(Verdict::Supported),
                                       alpha_increase_goal(), BaselineState::from_registry(reg));
    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("train the models"));
}

TEST_CASE("a Decrease goal ranks the opposite frequency to an Increase goal") {
    TempDir tmp;
    Spec sp;
    sp.peak_hz = 11.0;
    sp.peak_gain = 1.0;
    sp.noise_sd = 0.03;
    write_ml(tmp.path, sp);
    const ModelRegistry reg = train(tmp.path);

    DesiredState up = alpha_increase_goal();
    up.min_score = -1e9;
    DesiredState down = up;
    down.brain[0][index_of(Band::Alpha)].goal = Goal::Decrease;

    const auto ev = evidence_all(Verdict::Supported);
    const OptimizerResult a = optimize(reg, ev, up, BaselineState::from_registry(reg));
    const OptimizerResult b = optimize(reg, ev, down, BaselineState::from_registry(reg));

    REQUIRE_FALSE(a.ranked.empty());
    REQUIRE_FALSE(b.ranked.empty());
    // The peak is the best answer for "raise alpha" and the worst for "lower
    // it", so the two winners must not coincide.
    REQUIRE(std::abs(a.ranked.front().frequency_hz - b.ranked.front().frequency_hz) > 2.0);
}

TEST_CASE("scores are normalised so heart rate cannot dominate band power") {
    // BPM moves in units of ~5 and log band power in units of ~0.01. Without
    // dividing by each outcome's own spread, a heart goal would outweigh every
    // brain goal by two orders of magnitude regardless of the weights.
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    DesiredState d;
    d.require_effect_floor = false;
    d.min_score = -1e9;
    d.heart = {Goal::Increase, 0.0, 1.0};

    const OptimizerResult r =
        optimize(reg, evidence_all(Verdict::Supported), d, BaselineState::from_registry(reg));
    REQUIRE_FALSE(r.ranked.empty());
    // A normalised goal term stays within a few units; an unnormalised BPM term
    // would be in the tens or hundreds.
    for (const Candidate& c : r.ranked) REQUIRE(std::abs(c.goal_match) < 10.0);
}

TEST_CASE("evidence keys match the names the evidence gate produces") {
    // These two must agree exactly or every goal silently reads as unsupported.
    REQUIRE(evidence_key(ModelId::BrainS1, index_of(Band::Alpha), "d") ==
            "S1_d_logabs_alpha");
    REQUIRE(evidence_key(ModelId::BrainS4, index_of(Band::Gamma), "d") ==
            "S4_d_logabs_gamma");
    REQUIRE(evidence_key(ModelId::Heart, 0, "d") == "heart_d_bpm");
    REQUIRE(evidence_key(ModelId::Breathing, 0, "d") == "breath_d_breathing");
    REQUIRE(evidence_key(ModelId::BrainS2, index_of(Band::Beta), "post_d") ==
            "S2_post_d_logabs_beta");
}

TEST_CASE("a baseline far outside the training data is refused, not answered with zeros") {
    // The trap this guards. An RBF kernel evaluated tens of standard deviations
    // from every training point underflows to zero, so the GP falls back to its
    // prior mean at every frequency. The result is a perfectly flat curve of
    // zeros that looks like a confident finding of "nothing happens" rather
    // than what it is: the models have never seen a person in this state.
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    BaselineState wild = BaselineState::from_registry(reg);
    wild.brain_logabs[0][index_of(Band::Alpha)] += 50.0;

    const Prediction p = predict(reg, 11.0, wild);
    REQUIRE_FALSE(p.baseline_in_range());
    REQUIRE(p.max_baseline_z > 10.0);

    DesiredState d = alpha_increase_goal();
    d.min_score = -1e9;
    const OptimizerResult r = optimize(reg, evidence_all(Verdict::Supported), d, wild);
    REQUIRE_FALSE(r.found);
    REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("outside the range"));
}

TEST_CASE("the registry baseline sits inside the training range by construction") {
    TempDir tmp;
    write_ml(tmp.path, Spec{});
    const ModelRegistry reg = train(tmp.path);

    const Prediction p = predict(reg, 11.0, BaselineState::from_registry(reg));
    REQUIRE(p.baseline_in_range());
    REQUIRE(p.max_baseline_z < 1e-6);
}

TEST_CASE("the baseline distribution survives a save and load") {
    TempDir tmp, out;
    write_ml(tmp.path, Spec{});
    const ModelRegistry a = train(tmp.path);

    std::string err;
    REQUIRE(a.save(out.path, err));
    ModelRegistry b;
    REQUIRE(b.load(out.path, err));

    const int alpha = index_of(Band::Alpha);
    REQUIRE(b.output(ModelId::BrainS1, alpha).baseline_mean
            == Approx(a.output(ModelId::BrainS1, alpha).baseline_mean).margin(1e-9));
    REQUIRE(b.output(ModelId::BrainS1, alpha).baseline_sd
            == Approx(a.output(ModelId::BrainS1, alpha).baseline_sd).margin(1e-9));
}
