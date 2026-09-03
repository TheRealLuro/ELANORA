// The response math is the part of the pipeline that is easiest to get subtly
// wrong and hardest to notice: a raw difference instead of a log-ratio still
// produces plausible numbers that vary with frequency, and every model
// downstream would fit them happily. These tests pin the formulas to the
// schema table rather than to whatever the implementation happens to do.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>

#include "elanora/csv.hpp"
#include "elanora/data/dataset_builder.hpp"
#include "elanora/types.hpp"
#include "elanora/data/table.hpp"
#include "test_helpers.hpp"

using namespace elanora;
using namespace elanora::data;
using namespace elanora::test;
using Catch::Approx;

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("elanora_ds_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Writes a complete synthetic session: trials.csv plus raw streams whose alpha
// amplitude differs between periods by a known factor.
struct FixtureSpec {
    std::string trial_id  = "S_R01";
    std::string session   = "SESS1";
    std::string subject   = "P01";
    std::string condition = "stim";
    double frequency_hz   = 10.0;
    double baseline_amp   = 2.0;
    double stimulus_amp   = 4.0;
    double post_amp       = 2.0;
    double period_s       = 30.0;
};

void write_trial(const fs::path& root, const FixtureSpec& f, bool append) {
    const fs::path dir = root / "raw" / f.session;
    fs::create_directories(dir);

    const double t0 = 1000.0;
    const double t1 = t0 + f.period_s;
    const double t2 = t1 + f.period_s;
    const double t3 = t2 + f.period_s;

    {
        CsvWriter w(dir / (f.trial_id + "_markers.csv"), {"timestamp", "event"});
        w.row(std::vector<std::string>{fmt6(t0), "baseline_start"});
        w.row(std::vector<std::string>{fmt6(t1), "stimulus_start"});
        w.row(std::vector<std::string>{fmt6(t2), "post_start"});
        w.row(std::vector<std::string>{fmt6(t3), "trial_end"});
    }

    // EEG: a 10 Hz tone whose amplitude changes per period, plus light noise so
    // the other bands are not exactly zero.
    {
        const int sr = 256;
        CsvWriter w(dir / (f.trial_id + "_eeg.csv"),
                    {"timestamp", "TP9", "AF7", "AF8", "TP10"});
        std::mt19937_64 rng(42);
        std::normal_distribution<double> noise(0.0, 0.3);
        for (int p = 0; p < 3; ++p) {
            const double amp = (p == 0) ? f.baseline_amp
                             : (p == 1) ? f.stimulus_amp
                                        : f.post_amp;
            const double start = t0 + p * f.period_s;
            const int n = static_cast<int>(f.period_s * sr);
            for (int i = 0; i < n; ++i) {
                const double t = start + static_cast<double>(i) / sr;
                const double v = amp * std::sin(2.0 * kPi * 10.0 * i / sr);
                std::vector<std::string> row = {fmt6(t)};
                for (int c = 0; c < 4; ++c) row.push_back(fmt6(v + noise(rng)));
                w.row(row);
            }
        }
    }

    // PPG: a steady 1.2 Hz pulse in every period, so heart rows exist.
    {
        const int sr = 64;
        CsvWriter w(dir / (f.trial_id + "_ppg.csv"),
                    {"timestamp", "ppg_red", "ppg_ir", "ppg_ambient"});
        const int n = static_cast<int>(3 * f.period_s * sr);
        const std::vector<double> pulse = make_pulse_train(1.2, sr, 3 * f.period_s);
        for (int i = 0; i < n; ++i) {
            const double t = t0 + static_cast<double>(i) / sr;
            const double v = pulse[static_cast<std::size_t>(i)];
            w.row(std::vector<std::string>{fmt6(t), fmt6(v), fmt6(v), fmt6(0.1)});
        }
    }

    // IMU: a 0.2 Hz oscillation on ax, so breathing rows exist.
    {
        const int sr = 52;
        CsvWriter w(dir / (f.trial_id + "_imu.csv"),
                    {"timestamp", "ax", "ay", "az", "gx", "gy", "gz"});
        const int n = static_cast<int>(3 * f.period_s * sr);
        const std::vector<double> br = make_sine(0.2, sr, 3 * f.period_s, 0.05);
        for (int i = 0; i < n; ++i) {
            const double t = t0 + static_cast<double>(i) / sr;
            std::vector<std::string> row = {fmt6(t), fmt6(br[static_cast<std::size_t>(i)])};
            for (int c = 0; c < 5; ++c) row.push_back(fmt6(0.001));
            w.row(row);
        }
    }

    const std::vector<std::string> header = {
        "trial_id", "session_id", "subject_id", "round_index", "condition",
        "frequency_hz", "jitter_mean_hz", "started_at", "n_eeg", "n_ppg", "n_imu"};
    const std::vector<std::string> row = {
        f.trial_id, f.session, f.subject, "1", f.condition, fmt6(f.frequency_hz),
        "0", fmt6(t0), "0", "0", "0"};

    const fs::path tp = root / "trials.csv";
    if (append && fs::exists(tp)) {
        Table t = Table::read(tp);
        t.add_row(row);
        t.write(tp);
    } else {
        Table t(header);
        t.add_row(row);
        t.write(tp);
    }
}

Table build_and_read_brain(const fs::path& root) {
    build_features(root);
    build_ml_datasets(root);
    return Table::read(root / "ml" / "brain_dataset.csv");
}

}  // namespace

TEST_CASE("a two-trial fixture yields the expected long and wide row counts") {
    TempDir tmp;
    FixtureSpec a;
    FixtureSpec b;
    b.trial_id  = "S_R02";
    b.condition = "control_tone";
    b.frequency_hz = 0.0;
    write_trial(tmp.path, a, false);
    write_trial(tmp.path, b, true);

    const BuildReport rep = build_features(tmp.path);
    REQUIRE(rep.trials_seen == 2);
    REQUIRE(rep.trials_processed == 2);
    REQUIRE(rep.trials_skipped == 0);

    // 2 trials x 4 sensors x 3 periods.
    REQUIRE(rep.brain_rows == 24);
    REQUIRE(rep.heart_rows == 6);
    REQUIRE(rep.breath_rows == 6);

    build_ml_datasets(tmp.path);
    // Wide: one row per trial x sensor.
    REQUIRE(Table::read(tmp.path / "ml" / "brain_dataset.csv").rows() == 8u);
    REQUIRE(Table::read(tmp.path / "ml" / "heart_dataset.csv").rows() == 2u);
    REQUIRE(Table::read(tmp.path / "ml" / "breath_dataset.csv").rows() == 2u);
}

TEST_CASE("absolute response is a log-ratio, not a raw difference") {
    // Doubling power must give the same response value regardless of the level
    // it started from. A raw difference would give a bigger number for a
    // stronger electrode, and the models would learn electrode impedance.
    TempDir lo, hi;
    FixtureSpec weak;
    weak.baseline_amp = 2.0;
    weak.stimulus_amp = 2.0 * std::sqrt(2.0);   // power x2
    write_trial(lo.path, weak, false);

    FixtureSpec strong;
    strong.baseline_amp = 20.0;
    strong.stimulus_amp = 20.0 * std::sqrt(2.0);
    write_trial(hi.path, strong, false);

    const Table a = build_and_read_brain(lo.path);
    const Table b = build_and_read_brain(hi.path);

    const double da = a.num(0, "d_logabs_alpha");
    const double db = b.num(0, "d_logabs_alpha");

    REQUIRE(da == Approx(std::log(2.0)).margin(0.06));
    REQUIRE(db == Approx(std::log(2.0)).margin(0.06));
    // The two must agree with each other far more tightly than either agrees
    // with an arbitrary constant -- that is the scale invariance being tested.
    REQUIRE(da == Approx(db).margin(0.03));
}

TEST_CASE("CLR deltas sum to zero, documenting the limit rather than a bug") {
    // CLR is centred by construction, so the five values sum to zero and the
    // covariance is singular. CLR fixes the geometry of relative power -- it
    // becomes unbounded and ratio-additive -- but does not remove the linear
    // dependency. That is exactly why absolute log-power is the default
    // modelling target: only it can show alpha rising without mechanically
    // pushing some other band down.
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    const Table t = build_and_read_brain(tmp.path);

    for (const char* stage : {"d", "post_d", "recovery"}) {
        double sum = 0.0;
        for (int b = 0; b < kBandCount; ++b) {
            sum += t.num(0, std::string(stage) + "_clr_" + band_name(static_cast<Band>(b)));
        }
        REQUIRE(std::abs(sum) < 1e-6);
    }
}

TEST_CASE("the CLR of a period is itself centred") {
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    const Table t = build_and_read_brain(tmp.path);

    for (const char* stage : {"baseline", "stimulus", "post"}) {
        double sum = 0.0;
        for (int b = 0; b < kBandCount; ++b) {
            sum += t.num(0, std::string(stage) + "_clr_" + band_name(static_cast<Band>(b)));
        }
        REQUIRE(std::abs(sum) < 1e-5);
    }
}

TEST_CASE("recovery is post minus stimulus and post_d is post minus baseline") {
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    const Table t = build_and_read_brain(tmp.path);

    // Every field is stored at 6 decimal places, so a response read back from
    // the file and the same response recomputed from two stored operands can
    // differ by up to 1e-6 through rounding alone. The margin has to clear that
    // or the test fails on formatting rather than on arithmetic.
    constexpr double kRoundTrip = 5e-6;

    for (int b = 0; b < kBandCount; ++b) {
        const std::string band = band_name(static_cast<Band>(b));
        for (const char* rep : {"logabs", "clr"}) {
            const double base = t.num(0, "baseline_" + std::string(rep) + "_" + band);
            const double stim = t.num(0, "stimulus_" + std::string(rep) + "_" + band);
            const double post = t.num(0, "post_" + std::string(rep) + "_" + band);

            REQUIRE(t.num(0, "d_" + std::string(rep) + "_" + band)
                    == Approx(stim - base).margin(kRoundTrip));
            REQUIRE(t.num(0, "post_d_" + std::string(rep) + "_" + band)
                    == Approx(post - base).margin(kRoundTrip));
            REQUIRE(t.num(0, "recovery_" + std::string(rep) + "_" + band)
                    == Approx(post - stim).margin(kRoundTrip));
        }
    }
}

TEST_CASE("frequency reaches the models as log2, never as raw Hz") {
    TempDir tmp;
    FixtureSpec f;
    f.frequency_hz = 8.0;
    write_trial(tmp.path, f, false);
    const Table t = build_and_read_brain(tmp.path);
    REQUIRE(t.num(0, "log2_freq") == Approx(3.0).margin(1e-6));
    REQUIRE(t.num(0, "frequency_hz") == Approx(8.0));
}

TEST_CASE("a tone control has no frequency, so log2_freq is blank not zero") {
    // log2(0) is -inf. Writing it as 0.0 would place the tone control at 1 Hz
    // and quietly corrupt every fit that used it.
    TempDir tmp;
    FixtureSpec f;
    f.condition = "control_tone";
    f.frequency_hz = 0.0;
    write_trial(tmp.path, f, false);
    const Table t = build_and_read_brain(tmp.path);
    REQUIRE(t.is_blank(0, "log2_freq"));
    REQUIRE(t.get(0, "condition") == "control_tone");
}

TEST_CASE("a trial with incomplete markers is skipped and reported, never silently dropped") {
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    // Truncate the markers to a baseline only, as an aborted round would leave.
    const fs::path mp = tmp.path / "raw" / "SESS1" / "S_R01_markers.csv";
    {
        CsvWriter w(mp, {"timestamp", "event"});
        w.row(std::vector<std::string>{fmt6(1000.0), "baseline_start"});
    }

    const BuildReport rep = build_features(tmp.path);
    REQUIRE(rep.trials_seen == 1);
    REQUIRE(rep.trials_processed == 0);
    REQUIRE(rep.trials_skipped == 1);
    REQUIRE(rep.warnings.size() == 1u);
    REQUIRE(rep.warnings[0].find("incomplete markers") != std::string::npos);
}

TEST_CASE("the long feature table carries both representations and the dominance columns") {
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    build_features(tmp.path);
    const Table t = Table::read(tmp.path / "features" / "brain_features.csv");

    REQUIRE(t.has("abs_alpha"));
    REQUIRE(t.has("rel_alpha"));
    REQUIRE(t.has("total_power"));
    REQUIRE(t.has("dominant_band"));
    REQUIRE(t.has("dominance_margin"));
    REQUIRE(t.has("codominant_bands"));
    REQUIRE(t.has("quality_eeg"));

    // The fixture is a strong 10 Hz tone, so alpha must lead every row.
    for (std::size_t i = 0; i < t.rows(); ++i) {
        REQUIRE(t.get(i, "dominant_band") == "alpha");
    }

    // Relative powers sum to one; absolute ones carry the amplitude.
    double rel_sum = 0.0;
    for (int b = 0; b < kBandCount; ++b) {
        rel_sum += t.num(0, std::string("rel_") + band_name(static_cast<Band>(b)));
    }
    REQUIRE(rel_sum == Approx(1.0).margin(0.02));
}

TEST_CASE("every sensor and period appears exactly once per trial") {
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    build_features(tmp.path);
    const Table t = Table::read(tmp.path / "features" / "brain_features.csv");

    REQUIRE(t.unique("sensor").size() == 4u);
    REQUIRE(t.unique("period").size() == 3u);
    REQUIRE(t.unique("electrode").size() == 4u);
    REQUIRE(t.rows() == 12u);
}

TEST_CASE("heart and breathing land in physiologically sane ranges") {
    // A number outside these bounds means a DSP bug, not a discovery.
    TempDir tmp;
    write_trial(tmp.path, FixtureSpec{}, false);
    build_features(tmp.path);

    const Table h = Table::read(tmp.path / "features" / "heart_features.csv");
    for (std::size_t i = 0; i < h.rows(); ++i) {
        REQUIRE(h.num(i, "bpm") == Approx(72.0).margin(4.0));
        REQUIRE(h.num(i, "quality_heart") > 0.5);
    }

    const Table b = Table::read(tmp.path / "features" / "breath_features.csv");
    for (std::size_t i = 0; i < b.rows(); ++i) {
        REQUIRE(b.num(i, "breaths_per_minute") == Approx(12.0).margin(2.0));
        REQUIRE(b.get(i, "axis_used") == "ax");
    }
}

TEST_CASE("building over an empty tree reports the problem rather than throwing") {
    TempDir tmp;
    const BuildReport rep = build_features(tmp.path);
    REQUIRE(rep.trials_seen == 0);
    REQUIRE_FALSE(rep.ok());
    REQUIRE(rep.warnings.size() == 1u);

    const BuildReport ml = build_ml_datasets(tmp.path);
    REQUIRE_FALSE(ml.ok());
    REQUIRE(ml.warnings.size() == 1u);
}

TEST_CASE("clr matches its definition exactly") {
    const std::vector<double> x = {0.1, 0.2, 0.3, 0.15, 0.25};
    const std::vector<double> c = clr(x);

    double mean_log = 0.0;
    for (double v : x) mean_log += std::log(v + kLogEpsilon);
    mean_log /= static_cast<double>(x.size());

    for (std::size_t i = 0; i < x.size(); ++i) {
        REQUIRE(c[i] == Approx(std::log(x[i] + kLogEpsilon) - mean_log).margin(1e-12));
    }
    REQUIRE(std::accumulate(c.begin(), c.end(), 0.0) == Approx(0.0).margin(1e-12));
}

TEST_CASE("clr survives a zero-power band instead of returning negative infinity") {
    const std::vector<double> c = clr({0.5, 0.5, 0.0, 0.0, 0.0});
    for (double v : c) REQUIRE(std::isfinite(v));
}
