// The gate must be able to say no.
//
// The most important test in this file is the pure-noise one. With 22 outcomes
// at alpha 0.05, an uncorrected analysis would find roughly one "significant"
// result per run, and a gate that reliably passes on noise is worse than no
// gate at all -- it launders chance into apparent evidence.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>

#include "elanora/csv.hpp"
#include "elanora/data/evidence.hpp"
#include "elanora/data/table.hpp"

using namespace elanora;
using namespace elanora::data;
using Catch::Approx;

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("elanora_ev_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Builds ml/*.csv directly. Going through raw synthesis would make these tests
// minutes long and would test the feature layer again rather than the gate.
struct Fixture {
    int    sessions       = 4;
    int    subjects       = 2;
    double alpha_peak_hz  = 0.0;   // 0 disables the frequency effect
    double peak_gain      = 0.6;   // response amplitude at the peak
    double noise_sd       = 0.10;
    double control_shift  = 0.0;   // added to stimulus rows only
    uint64_t seed         = 1234;
};

const std::vector<double> kFreqs = {0.5, 0.71, 1.0, 1.41, 2.0, 2.83, 4.0,
                                    5.66, 8.0, 11.3, 16.0, 22.6, 32.0, 45.0};

void write_fixture(const fs::path& root, const Fixture& f) {
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

    std::vector<std::string> sh = {"subject_id", "session_id", "trial_id", "condition",
                                   "frequency_hz", "log2_freq"};
    for (const char* stage : {"baseline", "stimulus", "post", "d", "post_d", "recovery"}) {
        sh.push_back(std::string(stage) + "_bpm");
    }
    sh.push_back("baseline_rmssd");
    sh.push_back("d_rmssd");
    sh.push_back("quality_heart");
    Table heart(sh);

    std::vector<std::string> rh = {"subject_id", "session_id", "trial_id", "condition",
                                   "frequency_hz", "log2_freq"};
    for (const char* stage : {"baseline", "stimulus", "post", "d", "post_d", "recovery"}) {
        rh.push_back(std::string(stage) + "_breathing");
    }
    rh.push_back("baseline_breathing_spectral");
    rh.push_back("d_breathing_spectral");
    rh.push_back("confidence_breath");
    Table breath(rh);

    std::mt19937_64 rng(f.seed);
    std::normal_distribution<double> noise(0.0, f.noise_sd);

    for (int s = 0; s < f.sessions; ++s) {
        const std::string session = "SESS" + std::to_string(s);
        const std::string subject = "P" + std::to_string(s % f.subjects);

        int round = 0;
        auto emit = [&](const std::string& condition, double hz) {
            const std::string trial = session + "_R" + std::to_string(++round);
            const std::string log2f =
                (hz > 0.0) ? fmt6(std::log2(hz)) : std::string();

            // A Gaussian bump in log-frequency: the shape a real entrainment
            // response would have, and one a straight line cannot fit.
            double effect = 0.0;
            if (f.alpha_peak_hz > 0.0 && hz > 0.0 && condition == "stim") {
                const double dz = std::log2(hz) - std::log2(f.alpha_peak_hz);
                effect = f.peak_gain * std::exp(-0.5 * dz * dz / (0.6 * 0.6));
            }
            const double shift = (condition == "stim") ? f.control_shift : 0.0;

            for (int sensor = 0; sensor < kSensorCount; ++sensor) {
                const SensorId sid = static_cast<SensorId>(sensor);
                std::vector<std::string> row = {subject, session, trial, condition,
                                                fmt6(hz), log2f, sensor_name(sid),
                                                electrode_name(sid)};
                for (int rep = 0; rep < 2; ++rep) {
                    for (int stage = 0; stage < 6; ++stage) {
                        for (int b = 0; b < kBandCount; ++b) {
                            // Only S1 alpha carries the effect, so the test can
                            // check that it does not bleed into its neighbours.
                            const bool target = (sensor == 0 && b == index_of(Band::Alpha) &&
                                                 rep == 0);
                            double v = noise(rng);
                            if (target && stage == 3) v += effect + shift;   // "d"
                            if (target && stage == 0) v += 1.0;              // baseline level
                            row.push_back(fmt6(v));
                        }
                    }
                }
                row.push_back(fmt6(0.9));
                brain.add_row(std::move(row));
            }

            std::vector<std::string> hrow = {subject, session, trial, condition,
                                             fmt6(hz), log2f};
            for (int stage = 0; stage < 6; ++stage) hrow.push_back(fmt6(60.0 + noise(rng)));
            hrow.push_back(fmt6(30.0));
            hrow.push_back(fmt6(noise(rng)));
            hrow.push_back(fmt6(0.9));
            heart.add_row(std::move(hrow));

            std::vector<std::string> brow = {subject, session, trial, condition,
                                             fmt6(hz), log2f};
            for (int stage = 0; stage < 6; ++stage) brow.push_back(fmt6(12.0 + noise(rng)));
            brow.push_back(fmt6(12.0));
            brow.push_back(fmt6(noise(rng)));
            brow.push_back(fmt6(0.8));
            breath.add_row(std::move(brow));
        };

        for (double hz : kFreqs) emit("stim", hz);
        emit("control_jitter", 10.0);
        emit("control_jitter", 20.0);
        emit("control_tone", 0.0);
        emit("control_tone", 0.0);
    }

    brain.write(root / "ml" / "brain_dataset.csv");
    heart.write(root / "ml" / "heart_dataset.csv");
    breath.write(root / "ml" / "breath_dataset.csv");
}

const OutcomeEvidence& find(const std::vector<OutcomeEvidence>& ev, const std::string& name) {
    for (const OutcomeEvidence& e : ev) {
        if (e.outcome == name) return e;
    }
    throw std::runtime_error("outcome not found: " + name);
}

EvidenceOptions fast() {
    EvidenceOptions o;
    o.n_permutations = 300;   // enough to resolve p at the 0.05 boundary
    return o;
}

}  // namespace

TEST_CASE("the analysis reports exactly 22 outcomes in a fixed order") {
    TempDir tmp;
    write_fixture(tmp.path, Fixture{});
    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());

    REQUIRE(ev.size() == static_cast<std::size_t>(kOutcomeCount));
    REQUIRE(ev.size() == 22u);
    REQUIRE(ev.front().outcome == "S1_d_logabs_delta");
    REQUIRE(ev[19].outcome == "S4_d_logabs_gamma");
    REQUIRE(ev[20].family == "heart");
    REQUIRE(ev[21].family == "breath");
}

TEST_CASE("a pure noise dataset yields no supported outcome anywhere") {
    // The test that matters most. Everything downstream is gated on this
    // behaving correctly, because a gate that passes on noise would give the
    // optimizer's recommendations unearned authority.
    TempDir tmp;
    Fixture f;
    f.alpha_peak_hz = 0.0;    // no frequency effect
    f.control_shift = 0.0;    // no stimulus-versus-control difference
    write_fixture(tmp.path, f);

    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    REQUIRE(ev.size() == 22u);
    for (const OutcomeEvidence& e : ev) {
        REQUIRE(e.verdict != Verdict::Supported);
    }
    REQUIRE(summarize(ev).supported == 0);
    REQUIRE_FALSE(summarize(ev).any_supported());
}

TEST_CASE("a real frequency effect with a control difference is Supported") {
    TempDir tmp;
    Fixture f;
    f.alpha_peak_hz = 11.0;
    f.peak_gain     = 0.8;
    f.control_shift = 0.5;
    f.noise_sd      = 0.08;
    write_fixture(tmp.path, f);

    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    const OutcomeEvidence& a = find(ev, "S1_d_logabs_alpha");

    REQUIRE(a.tested());
    REQUIRE(a.verdict == Verdict::Supported);
    REQUIRE(a.cv_r2 > a.cv_r2_null_mean);
    REQUIRE(a.p_fdr < 0.05);
    REQUIRE(a.effect_stim_vs_control > 0.0);
}

TEST_CASE("an effect on one outcome does not leak into its neighbours") {
    TempDir tmp;
    Fixture f;
    f.alpha_peak_hz = 11.0;
    f.peak_gain     = 0.8;
    f.control_shift = 0.5;
    write_fixture(tmp.path, f);

    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    REQUIRE(find(ev, "S1_d_logabs_alpha").verdict == Verdict::Supported);
    // The fixture puts the effect only on S1 alpha; the same band on another
    // sensor and another band on the same sensor must stay unsupported.
    REQUIRE(find(ev, "S2_d_logabs_alpha").verdict != Verdict::Supported);
    REQUIRE(find(ev, "S1_d_logabs_beta").verdict != Verdict::Supported);
}

TEST_CASE("FDR correction is applied across all tested outcomes") {
    TempDir tmp;
    write_fixture(tmp.path, Fixture{});
    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    for (const OutcomeEvidence& e : ev) {
        if (!e.tested()) continue;
        REQUIRE(e.p_fdr >= e.p_raw - 1e-12);
        REQUIRE(e.p_fdr <= 1.0);
    }
}

TEST_CASE("a single session is reported as untestable, not as no evidence") {
    // These are different statements. "We looked and found nothing" and "we
    // could not look" must not render the same way to the person reading it.
    TempDir tmp;
    Fixture f;
    f.sessions = 1;
    write_fixture(tmp.path, f);

    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    for (const OutcomeEvidence& e : ev) {
        REQUIRE_FALSE(e.tested());
        REQUIRE(e.insufficient.find("one session") != std::string::npos);
    }
    REQUIRE(summarize(ev).untested == 22);
    REQUIRE(summarize(ev).none == 0);
}

TEST_CASE("an empty dataset produces 22 untestable outcomes rather than throwing") {
    TempDir tmp;
    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    REQUIRE(ev.size() == 22u);
    for (const OutcomeEvidence& e : ev) REQUIRE_FALSE(e.tested());
}

TEST_CASE("quality thresholds exclude rows per signal, not per trial") {
    TempDir tmp;
    Fixture f;
    f.alpha_peak_hz = 11.0;
    f.control_shift = 0.5;
    write_fixture(tmp.path, f);

    // Drop every EEG row below threshold; heart and breathing keep their rows.
    Table brain = Table::read(tmp.path / "ml" / "brain_dataset.csv");
    Table lowered(brain.header());
    for (std::size_t i = 0; i < brain.rows(); ++i) {
        std::vector<std::string> row = brain.row(i);
        row[static_cast<std::size_t>(brain.column("quality_eeg"))] = fmt6(0.2);
        lowered.add_row(std::move(row));
    }
    lowered.write(tmp.path / "ml" / "brain_dataset.csv");

    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    REQUIRE_FALSE(find(ev, "S1_d_logabs_alpha").tested());
    // The whole point of per-signal quality: heart still had usable rows.
    REQUIRE(find(ev, "heart_d_bpm").n_stim > 0);
}

TEST_CASE("evidence round-trips through its CSV") {
    TempDir tmp;
    Fixture f;
    f.alpha_peak_hz = 11.0;
    f.control_shift = 0.5;
    write_fixture(tmp.path, f);

    const std::vector<OutcomeEvidence> ev = analyze(tmp.path, fast());
    write_evidence(tmp.path, ev);
    REQUIRE(fs::exists(tmp.path / "analysis" / "evidence.csv"));

    const std::vector<OutcomeEvidence> back = read_evidence(tmp.path);
    REQUIRE(back.size() == ev.size());
    for (std::size_t i = 0; i < ev.size(); ++i) {
        REQUIRE(back[i].outcome == ev[i].outcome);
        REQUIRE(back[i].verdict == ev[i].verdict);
        REQUIRE(back[i].p_fdr == Approx(ev[i].p_fdr).margin(1e-6));
        REQUIRE(back[i].cv_r2 == Approx(ev[i].cv_r2).margin(1e-6));
    }
}

TEST_CASE("the analysis is reproducible from its seed") {
    TempDir tmp;
    Fixture f;
    f.alpha_peak_hz = 11.0;
    f.control_shift = 0.5;
    write_fixture(tmp.path, f);

    const std::vector<OutcomeEvidence> a = analyze(tmp.path, fast());
    const std::vector<OutcomeEvidence> b = analyze(tmp.path, fast());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].p_frequency == Approx(b[i].p_frequency).margin(1e-12));
        REQUIRE(a[i].cv_r2_null_mean == Approx(b[i].cv_r2_null_mean).margin(1e-12));
    }
}
