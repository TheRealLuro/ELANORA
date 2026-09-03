// End-to-end: synthesised raw recordings all the way to a recommendation.
//
// Every other test exercises one layer against hand-made inputs for that layer.
// This one starts where a real session starts -- raw EEG, PPG and IMU CSVs with
// markers -- and runs the whole chain:
//
//   raw -> build_features -> build_ml_datasets -> analyze -> train_all -> optimize
//
// It is the only test that would catch a column name agreed differently by two
// layers, and those are exactly the failures that survive unit tests and then
// silently produce an empty dataset on real hardware.
//
// The signal is deliberately simple: alpha amplitude rises for stimulus trials
// near 11 Hz. If the chain works, the evidence gate finds it on S1 alpha and
// the optimizer recommends a frequency near 11 Hz.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "elanora/csv.hpp"
#include "elanora/data/dataset_builder.hpp"
#include "elanora/data/evidence.hpp"
#include "elanora/data/table.hpp"
#include "elanora/models/model_registry.hpp"
#include "elanora/models/optimizer.hpp"

using namespace elanora;
using namespace elanora::models;
using elanora::data::Table;
using elanora::data::Verdict;
using Catch::Approx;

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Shorter than the real 30 s protocol so the test runs in seconds. Eight
// seconds at 256 Hz still gives Welch three half-overlapped 1024-point
// segments, which is enough for a stable band estimate.
constexpr double kPeriodS = 8.0;
constexpr int kSrEeg = 256;
constexpr int kSrPpg = 64;
constexpr int kSrImu = 52;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("elanora_e2e_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Alpha gain for a stimulus trial at `hz`: a bump centred on 11 Hz.
double alpha_gain(double hz, bool is_stim) {
    if (!is_stim || hz <= 0.0) return 1.0;
    const double dz = std::log2(hz) - std::log2(11.0);
    return 1.0 + 2.5 * std::exp(-0.5 * dz * dz / (0.5 * 0.5));
}

void write_trial(const fs::path& root, const std::string& session,
                 const std::string& subject, const std::string& trial,
                 const std::string& condition, double hz, std::mt19937_64& rng) {
    const fs::path dir = root / "raw" / session;
    fs::create_directories(dir);

    const double t0 = 1000.0;
    const double marks[4] = {t0, t0 + kPeriodS, t0 + 2 * kPeriodS, t0 + 3 * kPeriodS};

    {
        CsvWriter w(dir / (trial + "_markers.csv"), {"timestamp", "event"});
        w.row(std::vector<std::string>{fmt6(marks[0]), "baseline_start"});
        w.row(std::vector<std::string>{fmt6(marks[1]), "stimulus_start"});
        w.row(std::vector<std::string>{fmt6(marks[2]), "post_start"});
        w.row(std::vector<std::string>{fmt6(marks[3]), "trial_end"});
    }

    // ---- EEG --------------------------------------------------------------
    {
        std::normal_distribution<double> noise(0.0, 4.0);
        CsvWriter w(dir / (trial + "_eeg.csv"),
                    {"timestamp", "TP9", "AF7", "AF8", "TP10"});
        const int n = static_cast<int>(kPeriodS * kSrEeg);
        for (int p = 0; p < 3; ++p) {
            // Only the stimulus period is affected, so the response is a real
            // within-trial change rather than a between-trial offset.
            const double gain = (p == 1) ? alpha_gain(hz, condition == "stim") : 1.0;
            for (int i = 0; i < n; ++i) {
                const double t = marks[p] + static_cast<double>(i) / kSrEeg;
                const double phase = 2.0 * kPi * 10.0 * i / kSrEeg;
                std::vector<std::string> row = {fmt6(t)};
                for (int c = 0; c < 4; ++c) {
                    // S1 carries the effect; the other electrodes get a fixed
                    // alpha rhythm so they stay realistic but uninformative.
                    const double amp = (c == 0) ? 10.0 * gain : 10.0;
                    row.push_back(fmt6(amp * std::sin(phase) + noise(rng)));
                }
                w.row(row);
            }
        }
    }

    // ---- PPG --------------------------------------------------------------
    //
    // The rate differs per trial and shifts slightly between periods. An
    // identical pulse in every trial would make d_bpm exactly constant, and a
    // constant outcome is correctly refused as untestable -- which would make
    // this fixture exercise the refusal path instead of the heart model.
    {
        std::uniform_real_distribution<double> rate(1.05, 1.35);   // 63-81 BPM
        std::normal_distribution<double> drift(0.0, 0.02);
        const double base_hz = rate(rng);

        CsvWriter w(dir / (trial + "_ppg.csv"),
                    {"timestamp", "ppg_red", "ppg_ir", "ppg_ambient"});
        const int n_per = static_cast<int>(kPeriodS * kSrPpg);
        int idx = 0;
        for (int p = 0; p < 3; ++p) {
            const double hz_p = base_hz + drift(rng);
            const double period = kSrPpg / hz_p;
            const double width  = period * 0.25;
            for (int i = 0; i < n_per; ++i, ++idx) {
                const double t = t0 + static_cast<double>(idx) / kSrPpg;
                const double ph = std::fmod(static_cast<double>(i), period);
                const double v = (ph < width)
                                     ? 0.5 * (1.0 - std::cos(2.0 * kPi * ph / width))
                                     : 0.0;
                w.row(std::vector<std::string>{fmt6(t), fmt6(v), fmt6(v), fmt6(0.1)});
            }
        }
    }

    // ---- IMU --------------------------------------------------------------
    {
        CsvWriter w(dir / (trial + "_imu.csv"),
                    {"timestamp", "ax", "ay", "az", "gx", "gy", "gz"});
        const int n = static_cast<int>(3 * kPeriodS * kSrImu);
        for (int i = 0; i < n; ++i) {
            const double t = t0 + static_cast<double>(i) / kSrImu;
            const double br = 0.05 * std::sin(2.0 * kPi * 0.2 * i / kSrImu);
            std::vector<std::string> row = {fmt6(t), fmt6(br)};
            for (int c = 0; c < 5; ++c) row.push_back(fmt6(0.001));
            w.row(row);
        }
    }

    // ---- trials.csv -------------------------------------------------------
    const std::vector<std::string> header = {
        "trial_id", "session_id", "subject_id", "round_index", "condition",
        "frequency_hz", "jitter_mean_hz", "started_at", "n_eeg", "n_ppg", "n_imu"};
    const std::vector<std::string> row = {
        trial, session, subject, "1", condition, fmt6(hz), "0", fmt6(t0), "0", "0", "0"};

    const fs::path tp = root / "trials.csv";
    if (fs::exists(tp)) {
        Table t = Table::read(tp);
        t.add_row(row);
        t.write(tp);
    } else {
        Table t(header);
        t.add_row(row);
        t.write(tp);
    }
}

void write_study(const fs::path& root) {
    std::mt19937_64 rng(2026);
    const double freqs[] = {2.0, 4.0, 8.0, 11.3, 16.0, 32.0};

    for (int s = 0; s < 3; ++s) {
        const std::string session = "SESS" + std::to_string(s);
        const std::string subject = "P" + std::to_string(s % 2);
        int r = 0;
        for (double hz : freqs) {
            write_trial(root, session, subject,
                        session + "_R" + std::to_string(++r), "stim", hz, rng);
        }
        write_trial(root, session, subject, session + "_R" + std::to_string(++r),
                    "control_jitter", 10.0, rng);
        write_trial(root, session, subject, session + "_R" + std::to_string(++r),
                    "control_tone", 0.0, rng);
    }
}

}  // namespace

TEST_CASE("the whole chain runs from raw recordings to a recommendation", "[e2e]") {
    TempDir tmp;
    write_study(tmp.path);

    // ---- stage 1: raw -> long features ------------------------------------
    const elanora::data::BuildReport feat = elanora::data::build_features(tmp.path);
    INFO("warnings: " << (feat.warnings.empty() ? "none" : feat.warnings[0]));
    REQUIRE(feat.trials_seen == 24);
    REQUIRE(feat.trials_processed == 24);
    REQUIRE(feat.trials_skipped == 0);
    REQUIRE(feat.brain_rows == 24 * 4 * 3);
    REQUIRE(feat.heart_rows == 24 * 3);

    // Physiological sanity. A value outside these bounds means a DSP bug, not a
    // discovery -- the plan is explicit that this is the check to run first.
    const Table heart = Table::read(tmp.path / "features" / "heart_features.csv");
    for (std::size_t i = 0; i < heart.rows(); ++i) {
        REQUIRE(heart.num(i, "bpm") > 55.0);
        REQUIRE(heart.num(i, "bpm") < 90.0);
    }
    // Breathing is the signal this shortened protocol cannot support, and the
    // extractor says so rather than guessing. At 0.2 Hz an 8 s period holds
    // 1.6 cycles -- often too few for even two up-crossings -- against the six
    // the real 30 s period gives. The right assertion is therefore not that the
    // rate is correct but that the confidence is honestly low, which is exactly
    // what the cycle-count term in breath_from_imu exists to do.
    const Table breath = Table::read(tmp.path / "features" / "breath_features.csv");
    REQUIRE(breath.rows() > 0u);
    for (std::size_t i = 0; i < breath.rows(); ++i) {
        const double bpm = breath.num(i, "breaths_per_minute");
        REQUIRE(breath.num(i, "confidence_breath") < 0.5);
        // Where a rate is reported at all it must still be physiological; a
        // number outside this range would mean a DSP fault, not a short window.
        if (bpm > 0.0) {
            REQUIRE(bpm > 6.0);
            REQUIRE(bpm < 25.0);
        }
    }

    // And because that confidence is below the 0.5 threshold, those rows must
    // be excluded from the breathing model rather than quietly training it on
    // measurements the extractor already disowned.
    {
        elanora::data::EvidenceOptions probe;
        probe.n_permutations = 1;
        elanora::data::build_ml_datasets(tmp.path);
        ModelRegistry check;
        check.train_all(tmp.path);
        REQUIRE_FALSE(check.trained(ModelId::Breathing));
    }

    // ---- stage 2: long -> wide -------------------------------------------
    const elanora::data::BuildReport ml = elanora::data::build_ml_datasets(tmp.path);
    REQUIRE(ml.brain_rows == 24 * 4);

    const Table brain = Table::read(tmp.path / "ml" / "brain_dataset.csv");
    REQUIRE(brain.has("d_logabs_alpha"));
    REQUIRE(brain.has("log2_freq"));

    // The planted effect must survive the whole extraction: S1 alpha should
    // rise more at 11.3 Hz than at 2 Hz.
    double at_peak = 0.0, at_edge = 0.0;
    int n_peak = 0, n_edge = 0;
    for (std::size_t i = 0; i < brain.rows(); ++i) {
        if (brain.get(i, "sensor") != "S1") continue;
        if (brain.get(i, "condition") != "stim") continue;
        const double hz = brain.num(i, "frequency_hz");
        const double d = brain.num(i, "d_logabs_alpha");
        if (std::abs(hz - 11.3) < 0.1) { at_peak += d; ++n_peak; }
        if (std::abs(hz - 2.0) < 0.1)  { at_edge += d; ++n_edge; }
    }
    REQUIRE(n_peak == 3);
    REQUIRE(n_edge == 3);
    REQUIRE(at_peak / n_peak > at_edge / n_edge);

    // ---- stage 3: the evidence gate --------------------------------------
    elanora::data::EvidenceOptions eo;
    eo.n_permutations = 200;
    const std::vector<elanora::data::OutcomeEvidence> ev =
        elanora::data::analyze(tmp.path, eo);
    REQUIRE(ev.size() == 22u);
    elanora::data::write_evidence(tmp.path, ev);

    const elanora::data::OutcomeEvidence* alpha = nullptr;
    for (const auto& e : ev) {
        if (e.outcome == "S1_d_logabs_alpha") alpha = &e;
    }
    REQUIRE(alpha != nullptr);
    INFO("S1 alpha: p_fdr=" << alpha->p_fdr << " cv_r2=" << alpha->cv_r2
                            << " null=" << alpha->cv_r2_null_mean
                            << " reason=" << alpha->insufficient);
    REQUIRE(alpha->tested());
    REQUIRE(alpha->verdict != Verdict::NoEvidence);

    // ---- stage 4: training ------------------------------------------------
    ModelRegistry reg;
    reg.train_all(tmp.path);
    INFO("brain reason: " << reg.untrained_reason(ModelId::BrainS1));
    REQUIRE(reg.trained(ModelId::BrainS1));
    // Heart trains: its rate genuinely varies between trials. Breathing does
    // not, for a different and equally honest reason -- see above. Per-signal
    // quality is exactly what lets one succeed while the other is excluded.
    REQUIRE(reg.trained(ModelId::Heart));
    // Breathing stays untrained here for the reason established above: its
    // confidence never clears the threshold at an 8 s period. Per-signal
    // quality is what lets the brain and heart models train anyway.
    REQUIRE_FALSE(reg.trained(ModelId::Breathing));
    REQUIRE_FALSE(reg.untrained_reason(ModelId::Breathing).empty());

    const auto [lo, hi] = reg.trained_freq_range();
    REQUIRE(lo == Approx(2.0));
    REQUIRE(hi == Approx(32.0));

    // ---- stage 5: forward -------------------------------------------------
    const BaselineState base = BaselineState::from_registry(reg);
    const Prediction p = predict(reg, 11.3, base);
    REQUIRE(p.baseline_in_range());
    const auto ai = static_cast<std::size_t>(index_of(Band::Alpha));
    REQUIRE(std::isfinite(p.d_bands[0][ai]));

    // ---- stage 6: inverse -------------------------------------------------
    DesiredState want;
    want.brain[0][index_of(Band::Alpha)] = {Goal::Increase, 0.0, 1.0};
    want.require_effect_floor = false;
    want.min_score = 0.0;

    const OptimizerResult r = optimize(reg, ev, want, base);
    INFO("optimizer: " << r.reason);

    if (alpha->verdict == Verdict::Supported) {
        REQUIRE(r.found);
        // The recommendation must land near where the effect was planted.
        REQUIRE(r.ranked.front().frequency_hz > 5.0);
        REQUIRE(r.ranked.front().frequency_hz < 24.0);
        // And never outside the range anything was actually presented at.
        for (const Candidate& c : r.ranked) {
            REQUIRE(c.frequency_hz >= 2.0 - 1e-9);
            REQUIRE(c.frequency_hz <= 32.0 + 1e-9);
        }
    } else {
        // A Weak verdict is still gated. That is correct behaviour, and the
        // reason must say so rather than silently returning nothing.
        REQUIRE_FALSE(r.found);
        REQUIRE_THAT(r.reason, Catch::Matchers::ContainsSubstring("no evidence"));
    }
}

TEST_CASE("a study with no planted effect produces no recommendation", "[e2e]") {
    // The negative control for the whole pipeline. Same shape of data, same
    // number of trials, no relationship between frequency and response -- and
    // the system must decline to recommend anything.
    TempDir tmp;
    std::mt19937_64 rng(99);
    const double freqs[] = {2.0, 4.0, 8.0, 11.3, 16.0, 32.0};
    for (int s = 0; s < 3; ++s) {
        const std::string session = "SESS" + std::to_string(s);
        int r = 0;
        for (double hz : freqs) {
            // condition "stim" but written through the control path, so the
            // alpha gain stays at 1.0 regardless of frequency.
            write_trial(tmp.path, session, "P" + std::to_string(s % 2),
                        session + "_R" + std::to_string(++r), "stim", 0.0, rng);
            // Re-label the row's frequency so the design is intact but the
            // response carries no frequency information.
            Table t = Table::read(tmp.path / "trials.csv");
            std::vector<std::string> row = t.row(t.rows() - 1);
            row[static_cast<std::size_t>(t.column("frequency_hz"))] = fmt6(hz);
            Table fixed(t.header());
            for (std::size_t i = 0; i + 1 < t.rows(); ++i) fixed.add_row(t.row(i));
            fixed.add_row(row);
            fixed.write(tmp.path / "trials.csv");
        }
        write_trial(tmp.path, session, "P" + std::to_string(s % 2),
                    session + "_R" + std::to_string(++r), "control_tone", 0.0, rng);
        write_trial(tmp.path, session, "P" + std::to_string(s % 2),
                    session + "_R" + std::to_string(++r), "control_jitter", 10.0, rng);
    }

    elanora::data::build_features(tmp.path);
    elanora::data::build_ml_datasets(tmp.path);

    elanora::data::EvidenceOptions eo;
    eo.n_permutations = 200;
    const std::vector<elanora::data::OutcomeEvidence> ev =
        elanora::data::analyze(tmp.path, eo);

    ModelRegistry reg;
    reg.train_all(tmp.path);

    DesiredState want;
    want.brain[0][index_of(Band::Alpha)] = {Goal::Increase, 0.0, 1.0};

    const OptimizerResult r =
        optimize(reg, ev, want, BaselineState::from_registry(reg));
    INFO("optimizer said: " << r.reason);
    REQUIRE_FALSE(r.found);
    REQUIRE_FALSE(r.reason.empty());
}
