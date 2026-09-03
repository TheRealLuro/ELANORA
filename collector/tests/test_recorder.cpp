// End-to-end check that a round actually reaches disk with the schema the
// analysis layer expects. The trial runner and the tone generator are already
// covered; this is the third leg -- without it a working UI can still silently
// record nothing, which is the one failure mode that costs a real subject's
// whole session.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "elanora/collector/recorder.hpp"
#include "elanora/csv.hpp"

using namespace elanora;
using namespace elanora::collector;

namespace {

namespace fs = std::filesystem;

// A scratch tree that removes itself, so a failing assertion cannot leave the
// next run reading a previous run's files.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("elanora_rec_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::vector<lsl::Sample> make_samples(int n, int n_channels, double t0, double dt) {
    std::vector<lsl::Sample> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        lsl::Sample s;
        s.ts = t0 + i * dt;
        s.values.assign(static_cast<std::size_t>(n_channels), 0.0);
        // Column c carries c*1000 + i, so a transposed or off-by-one write is
        // visible in the file rather than merely plausible.
        for (int c = 0; c < n_channels; ++c) {
            s.values[static_cast<std::size_t>(c)] = c * 1000.0 + i;
        }
        out.push_back(std::move(s));
    }
    return out;
}

lsl::ChannelMap muse_like_map() {
    lsl::ChannelMap ch;
    ch.eeg    = {1, 2, 3, 4};
    ch.ppg    = {1, 2, 3};
    ch.accel  = {1, 2, 3};
    ch.gyro   = {4, 5, 6};
    ch.sr_eeg = 256;
    ch.sr_ppg = 64;
    ch.sr_imu = 52;
    return ch;
}

// Reads a CSV so assertions can name columns instead of counting them.
struct Table {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;

    int col(const std::string& name) const {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == name) return static_cast<int>(i);
        }
        return -1;
    }
    std::string at(std::size_t r, const std::string& name) const {
        const int c = col(name);
        if (c < 0 || r >= rows.size()) return {};
        return rows[r][static_cast<std::size_t>(c)];
    }
};

Table read_table(const fs::path& p) {
    Table t;
    CsvReader r(p);
    t.header = r.header();
    std::vector<std::string> row;
    while (r.next(row)) t.rows.push_back(row);
    return t;
}

StimulusDesign basic_design() {
    StimulusDesign d;
    d.carrier_hz = 440.0;
    d.duty       = 0.5;
    return d;
}

}  // namespace

TEST_CASE("a round reaches disk with every stream and the schema headers") {
    TempDir tmp;
    TrialRecorder rec;
    std::string err;

    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P01", 1, basic_design(), d, 1234, 18, err));
    REQUIRE(err.empty());
    REQUIRE(rec.active());

    PlannedRound round;
    round.cond = Condition::Stim;
    round.hz   = 10.0;

    const std::vector<Marker> markers = {
        {100.0, "baseline_start"},
        {130.0, "stimulus_start"},
        {160.0, "post_start"},
        {190.0, "trial_end"},
    };

    REQUIRE(rec.write_round(0, round,
                            make_samples(256, 5, 100.0, 1.0 / 256.0),
                            make_samples(64, 4, 100.0, 1.0 / 64.0),
                            make_samples(52, 7, 100.0, 1.0 / 52.0),
                            markers, muse_like_map(), err));
    REQUIRE(err.empty());
    REQUIRE(rec.rounds_written() == 1);

    const fs::path dir  = rec.session_dir();
    const std::string tid = rec.session_id() + "_R01";

    REQUIRE(fs::exists(dir / (tid + "_eeg.csv")));
    REQUIRE(fs::exists(dir / (tid + "_ppg.csv")));
    REQUIRE(fs::exists(dir / (tid + "_imu.csv")));
    REQUIRE(fs::exists(dir / (tid + "_markers.csv")));

    SECTION("the EEG file carries the four electrodes in the fixed order") {
        const Table t = read_table(dir / (tid + "_eeg.csv"));
        const std::vector<std::string> want = {"timestamp", "TP9", "AF7", "AF8", "TP10"};
        REQUIRE(t.header == want);
        REQUIRE(t.rows.size() == 256u);
        // Sample channel 1 became column TP9, so its first value is 1000.
        REQUIRE(t.at(0, "TP9") == fmt6(1000.0));
        REQUIRE(t.at(0, "TP10") == fmt6(4000.0));
    }

    SECTION("PPG keeps all three channels including the ambient reference") {
        const Table t = read_table(dir / (tid + "_ppg.csv"));
        const std::vector<std::string> want = {"timestamp", "ppg_red", "ppg_ir", "ppg_ambient"};
        REQUIRE(t.header == want);
        REQUIRE(t.rows.size() == 64u);
    }

    SECTION("IMU keeps six axes") {
        const Table t = read_table(dir / (tid + "_imu.csv"));
        REQUIRE(t.header.size() == 7u);
        REQUIRE(t.rows.size() == 52u);
    }

    SECTION("markers land in order with the schema's event names") {
        const Table t = read_table(dir / (tid + "_markers.csv"));
        REQUIRE(t.rows.size() == 4u);
        REQUIRE(t.at(0, "event") == "baseline_start");
        REQUIRE(t.at(3, "event") == "trial_end");
        // The 30 s phase spacing must survive the write; a rounded or dropped
        // timestamp here would silently shift every feature window later.
        REQUIRE(t.at(1, "timestamp") == fmt6(130.0));
    }

    SECTION("the trial row carries the condition and frequency") {
        const Table t = read_table(tmp.path / "trials.csv");
        REQUIRE(t.rows.size() == 1u);
        REQUIRE(t.at(0, "condition") == "stim");
        REQUIRE(t.at(0, "frequency_hz") == fmt6(10.0));
        REQUIRE(t.at(0, "subject_id") == "P01");
        REQUIRE(t.at(0, "n_eeg") == "256");
    }
}

TEST_CASE("rounds append rather than overwrite, so a crash costs one round") {
    TempDir tmp;
    TrialRecorder rec;
    std::string err;
    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P02", 1, basic_design(), d, 7, 3, err));

    for (int i = 0; i < 3; ++i) {
        PlannedRound r;
        r.cond = (i == 1) ? Condition::ControlTone : Condition::Stim;
        r.hz   = 4.0 * (i + 1);
        REQUIRE(rec.write_round(i, r, make_samples(8, 5, 0.0, 0.01), {}, {},
                                {{0.0, "baseline_start"}}, muse_like_map(), err));
    }

    const Table t = read_table(tmp.path / "trials.csv");
    REQUIRE(t.rows.size() == 3u);
    REQUIRE(t.at(0, "round_index") == "1");
    REQUIRE(t.at(2, "round_index") == "3");
    REQUIRE(t.at(1, "condition") == "control_tone");
    REQUIRE(fs::exists(rec.session_dir() / (rec.session_id() + "_R03_eeg.csv")));
}

TEST_CASE("an absent stream is written as no file rather than an empty one") {
    // The Muse can drop a preset entirely. That must not produce a zero-row
    // CSV, which downstream code would read as "recorded and flat".
    TempDir tmp;
    TrialRecorder rec;
    std::string err;
    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P03", 1, basic_design(), d, 1, 1, err));

    PlannedRound r;
    r.cond = Condition::Stim;
    r.hz   = 8.0;
    REQUIRE(rec.write_round(0, r, make_samples(8, 5, 0.0, 0.01), {}, {},
                            {{0.0, "baseline_start"}}, muse_like_map(), err));

    const std::string tid = rec.session_id() + "_R01";
    REQUIRE(fs::exists(rec.session_dir() / (tid + "_eeg.csv")));
    REQUIRE_FALSE(fs::exists(rec.session_dir() / (tid + "_ppg.csv")));
}

TEST_CASE("the survey row is keyed to the same trial id as the round") {
    TempDir tmp;
    TrialRecorder rec;
    std::string err;
    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P04", 2, basic_design(), d, 99, 1, err));

    PlannedRound r;
    r.cond = Condition::Stim;
    r.hz   = 16.0;
    REQUIRE(rec.write_round(0, r, make_samples(4, 5, 0.0, 0.01), {}, {},
                            {{0.0, "baseline_start"}}, muse_like_map(), err));

    Survey s;
    s.relaxation   = 5;
    s.alertness    = 3;
    s.pleasantness = 6;
    s.discomfort   = 1;
    s.rhythm       = 0;   // heard it clearly
    s.breathing_perceived = 0;   // slower
    s.breaths_self_count  = 14;
    s.jaw  = true;
    s.note = "yawned near the end";
    REQUIRE(rec.write_survey(0, s, err));

    const Table trials  = read_table(tmp.path / "trials.csv");
    const Table surveys = read_table(tmp.path / "surveys.csv");
    REQUIRE(surveys.rows.size() == 1u);
    // The join key is the whole point of the file; if these ever diverge the
    // self-report cannot be attached to the physiology.
    REQUIRE(surveys.at(0, "trial_id") == trials.at(0, "trial_id"));
    REQUIRE(surveys.at(0, "relaxation") == "5");
    REQUIRE(surveys.at(0, "pleasantness") == "6");
    REQUIRE(surveys.at(0, "discomfort") == "1");
    REQUIRE(surveys.at(0, "heard_rhythm") == "clear");
    REQUIRE(surveys.at(0, "breathing_perceived") == "slower");
    REQUIRE(surveys.at(0, "breaths_self_count") == "14");
    REQUIRE(surveys.at(0, "artifact_jaw") == "1");
    // A comma in free text must survive quoting rather than splitting the row.
    REQUIRE(surveys.at(0, "note") == "yawned near the end");
}

TEST_CASE("an uncounted breath total is blank, never zero") {
    // Zero breaths in ninety seconds is a real number, and a downstream mean
    // would average it in. "Not counted" has to stay distinguishable.
    TempDir tmp;
    TrialRecorder rec;
    std::string err;
    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P05", 1, basic_design(), d, 3, 1, err));

    PlannedRound r;
    r.cond = Condition::Stim;
    r.hz   = 8.0;
    REQUIRE(rec.write_round(0, r, make_samples(4, 5, 0.0, 0.01), {}, {},
                            {{0.0, "baseline_start"}}, muse_like_map(), err));

    Survey s;
    s.relaxation = 4;
    s.alertness  = 4;
    s.rhythm     = 2;
    s.breaths_self_count = -1;      // not counted
    REQUIRE(rec.write_survey(0, s, err));

    const Table t = read_table(tmp.path / "surveys.csv");
    REQUIRE(t.at(0, "breaths_self_count").empty());
    REQUIRE(t.at(0, "heard_rhythm") == "none");
    REQUIRE(t.at(0, "breathing_perceived").empty());
}

TEST_CASE("a comma in the free note does not split the row") {
    TempDir tmp;
    TrialRecorder rec;
    std::string err;
    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P06", 1, basic_design(), d, 4, 1, err));

    PlannedRound r;
    r.cond = Condition::Stim;
    r.hz   = 8.0;
    REQUIRE(rec.write_round(0, r, make_samples(4, 5, 0.0, 0.01), {}, {},
                            {{0.0, "baseline_start"}}, muse_like_map(), err));

    Survey s;
    s.relaxation = 4;
    s.alertness  = 4;
    s.rhythm     = 1;
    s.note = "door slammed, then a phone rang";
    REQUIRE(rec.write_survey(0, s, err));

    const Table t = read_table(tmp.path / "surveys.csv");
    REQUIRE(t.rows.size() == 1u);
    REQUIRE(t.at(0, "note") == "door slammed, then a phone rang");
}

TEST_CASE("starting with a bad electrode is recorded in the session row") {
    // The operator may override the electrode gate. The trial is still worth
    // recording -- but the analysis has to be able to see that it happened.
    TempDir tmp;
    TrialRecorder rec;
    std::string err;
    Durations d;
    REQUIRE(rec.begin_trial(tmp.path, "P07", 1, basic_design(), d, 5, 1, err,
                            /*quality_override=*/true));

    const Table t = read_table(tmp.path / "sessions.csv");
    REQUIRE(t.at(0, "quality_override") == "1");
}

TEST_CASE("writing without an open trial is refused rather than crashing") {
    TrialRecorder rec;
    std::string err;
    PlannedRound r;
    REQUIRE_FALSE(rec.write_round(0, r, {}, {}, {}, {}, muse_like_map(), err));
    REQUIRE_FALSE(err.empty());

    Survey s;
    REQUIRE_FALSE(rec.write_survey(0, s, err));
}

