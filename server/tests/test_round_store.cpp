#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "elanora/server/round_store.hpp"

using namespace elanora;
using namespace elanora::server;

namespace {

namespace fs = std::filesystem;

// A scratch tree that removes itself, so a failing assertion cannot leave the
// next run reading a previous run's files.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() /
               ("elanora_srv_" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Shared by the cases below so no test has to say "same as above".
RoundUpload sample_round() {
    RoundUpload r;
    r.session_id = "P01_S01_20260903-120000";
    r.trial_id = r.session_id + "_R01";
    r.subject_id = "P01";
    r.condition = "stim";
    r.round_index = 1;
    r.frequency_hz = 11.3;
    r.eeg_csv = "timestamp,TP9,AF7,AF8,TP10\n0.000000,1.0,2.0,3.0,4.0\n";
    r.ppg_csv = "timestamp,ppg_red,ppg_ir,ppg_ambient\n0.000000,1,2,3\n";
    r.imu_csv = "timestamp,ax,ay,az,gx,gy,gz\n0.000000,0,0,1,0,0,0\n";
    r.markers_csv = "timestamp,event\n0.000000,baseline_start\n";
    return r;
}

int count_lines(const fs::path& p) {
    std::ifstream in(p);
    int n = 0;
    std::string line;
    while (std::getline(in, line)) ++n;
    return n;
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("a stored round produces the schema files", "[server][store]") {
    TempDir tmp;
    const RoundUpload r = sample_round();
    std::string err;
    REQUIRE(store_round(tmp.path, r, err));
    REQUIRE(err.empty());

    const auto dir = tmp.path / "raw" / r.session_id;
    REQUIRE(fs::exists(dir / (r.trial_id + "_eeg.csv")));
    REQUIRE(fs::exists(dir / (r.trial_id + "_ppg.csv")));
    REQUIRE(fs::exists(dir / (r.trial_id + "_imu.csv")));
    REQUIRE(fs::exists(dir / (r.trial_id + "_markers.csv")));
    REQUIRE(fs::exists(tmp.path / "trials.csv"));
}

TEST_CASE("a round with the wrong EEG header is rejected", "[server][store]") {
    // A malformed upload must not be able to write a file that later makes
    // elanora_data read wrong numbers -- a far worse failure than a rejected
    // upload, because nothing announces it.
    TempDir tmp;
    RoundUpload r = sample_round();
    r.eeg_csv = "timestamp,WRONG\n0.000000,1.0\n";
    std::string err;
    REQUIRE_FALSE(store_round(tmp.path, r, err));
    REQUIRE(err.find("TP9") != std::string::npos);
    REQUIRE_FALSE(fs::exists(tmp.path / "trials.csv"));
}

TEST_CASE("two rounds append rather than overwrite trials.csv", "[server][store]") {
    TempDir tmp;
    RoundUpload r = sample_round();
    std::string err;
    r.trial_id = r.session_id + "_R01";
    REQUIRE(store_round(tmp.path, r, err));
    r.trial_id = r.session_id + "_R02";
    r.round_index = 2;
    REQUIRE(store_round(tmp.path, r, err));

    // Header plus two rows. Raw data is never overwritten.
    REQUIRE(count_lines(tmp.path / "trials.csv") == 3);
}

TEST_CASE("re-uploading the same round is refused, not silently replaced",
          "[server][store]") {
    // The phone retries on a flaky connection. Overwriting would lose whichever
    // copy was the real one.
    TempDir tmp;
    const RoundUpload r = sample_round();
    std::string err;
    REQUIRE(store_round(tmp.path, r, err));
    REQUIRE_FALSE(store_round(tmp.path, r, err));
    REQUIRE(err.find("already stored") != std::string::npos);
    REQUIRE(count_lines(tmp.path / "trials.csv") == 2);
}

TEST_CASE("a session id cannot escape the dataset directory", "[server][store]") {
    // session_id arrives over the network and becomes a directory name.
    TempDir tmp;
    RoundUpload r = sample_round();
    r.session_id = "../../evil";
    std::string err;
    REQUIRE_FALSE(store_round(tmp.path, r, err));
    REQUIRE(err.find("session_id") != std::string::npos);
}

TEST_CASE("the trials row records the rows that actually landed",
          "[server][store]") {
    TempDir tmp;
    RoundUpload r = sample_round();
    r.eeg_csv = "timestamp,TP9,AF7,AF8,TP10\n0.0,1,2,3,4\n0.1,1,2,3,4\n0.2,1,2,3,4\n";
    std::string err;
    REQUIRE(store_round(tmp.path, r, err));

    const std::string trials = read_all(tmp.path / "trials.csv");
    // n_eeg is counted from the payload, not taken on trust from the phone.
    // Not anchored to the end of the row, so adding a column later does not
    // break an assertion that is about the counts.
    REQUIRE(trials.find(",3,1,1,") != std::string::npos);
}

TEST_CASE("started_at comes from the first marker", "[server][store]") {
    TempDir tmp;
    RoundUpload r = sample_round();
    r.markers_csv = "timestamp,event\n123.456789,baseline_start\n153.5,stimulus_start\n";
    std::string err;
    REQUIRE(store_round(tmp.path, r, err));
    REQUIRE(read_all(tmp.path / "trials.csv").find("123.456789") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------

namespace {

std::string envelope(const std::string& headers,
                     const std::string& eeg, const std::string& markers) {
    std::string body = headers + "\n";
    body += "@eeg " + std::to_string(eeg.size()) + "\n" + eeg;
    body += "@markers " + std::to_string(markers.size()) + "\n" + markers;
    return body;
}

const char* kHeaders =
    "session_id: P01_S01_20260903-120000\n"
    "trial_id: P01_S01_20260903-120000_R01\n"
    "subject_id: P01\n"
    "condition: stim\n"
    "round_index: 1\n"
    "frequency_hz: 11.300000\n";

}  // namespace

TEST_CASE("a well-formed envelope parses", "[server][wire]") {
    const std::string eeg = "timestamp,TP9,AF7,AF8,TP10\n0.000000,1,2,3,4\n";
    const std::string mk = "timestamp,event\n0.000000,baseline_start\n";
    RoundUpload r;
    std::string err;
    REQUIRE(parse_round(envelope(kHeaders, eeg, mk), r, err));
    REQUIRE(r.session_id == "P01_S01_20260903-120000");
    REQUIRE(r.condition == "stim");
    REQUIRE(r.round_index == 1);
    REQUIRE(r.eeg_csv == eeg);
    REQUIRE(r.markers_csv == mk);
}

TEST_CASE("payload commas and newlines need no escaping", "[server][wire]") {
    // The reason this is not JSON: the payload is 400 KB of commas and
    // newlines, and an escaping bug would corrupt samples in ways that still
    // parse as a valid CSV.
    const std::string eeg =
        "timestamp,TP9,AF7,AF8,TP10\n" + std::string(200, '\n');
    RoundUpload r;
    std::string err;
    REQUIRE(parse_round(envelope(kHeaders, eeg, ""), r, err));
    REQUIRE(r.eeg_csv == eeg);
}

TEST_CASE("a truncated upload is rejected, not stored as a short round",
          "[server][wire]") {
    // Storing the prefix would look like a successful round that happens to
    // end early -- indistinguishable later from a real dropout.
    std::string body = std::string(kHeaders) + "\n@eeg 5000\nshort";
    RoundUpload r;
    std::string err;
    REQUIRE_FALSE(parse_round(body, r, err));
    REQUIRE(err.find("5000") != std::string::npos);
}

TEST_CASE("an unlabelled trial is refused", "[server][wire]") {
    // There is no such thing as an unlabelled trial: the whole design rests on
    // comparing stimulus against control.
    std::string body =
        "session_id: S\ntrial_id: T\ncondition: \n\n@eeg 0\n";
    RoundUpload r;
    std::string err;
    REQUIRE_FALSE(parse_round(body, r, err));
    REQUIRE(err.find("condition") != std::string::npos);
}

TEST_CASE("an unknown header is ignored so an older server still works",
          "[server][wire]") {
    std::string h = std::string(kHeaders) + "future_field: 42\n";
    RoundUpload r;
    std::string err;
    REQUIRE(parse_round(envelope(h, "", ""), r, err));
    REQUIRE(r.subject_id == "P01");
}

TEST_CASE("the suspect flag round-trips", "[server][wire]") {
    std::string h = std::string(kHeaders) + "suspect: 1\n";
    RoundUpload r;
    std::string err;
    REQUIRE(parse_round(envelope(h, "", ""), r, err));
    REQUIRE(r.suspect);
}

// ---------------------------------------------------------------------------
// Surveys and session headers
// ---------------------------------------------------------------------------

TEST_CASE("a survey row lands in the declared column order", "[server][survey]") {
    TempDir tmp;
    // Deliberately out of order, and missing one field, because the phone
    // builds this map and JS object order is not a guarantee worth relying on.
    const std::string body =
        "heard_rhythm: clear\n"
        "trial_id: P01_S01_R01\n"
        "relaxation: 5\n"
        "session_id: P01_S01\n"
        "subject_id: P01\n"
        "round_index: 1\n"
        "note: felt calm\n";
    std::string err;
    REQUIRE(store_survey(tmp.path, body, err));

    const std::string csv = read_all(tmp.path / "surveys.csv");
    REQUIRE(csv.find("trial_id,session_id,subject_id,round_index,relaxation") == 0);
    // trial_id first, then session, subject, index, relaxation -- schema order,
    // not the order the fields arrived in.
    REQUIRE(csv.find("P01_S01_R01,P01_S01,P01,1,5,") != std::string::npos);
}

TEST_CASE("an absent survey field becomes an empty cell, not a dropped column",
          "[server][survey]") {
    // A row with the wrong number of columns misaligns every value after the
    // gap and still parses, which is the worst kind of failure here.
    TempDir tmp;
    std::string err;
    REQUIRE(store_survey(tmp.path, "trial_id: T\nrelaxation: 5\n", err));

    const std::string csv = read_all(tmp.path / "surveys.csv");
    const std::size_t nl = csv.find('\n');
    const std::string header = csv.substr(0, nl);
    const std::string row = csv.substr(nl + 1);

    const auto commas = [](const std::string& s) {
        return std::count(s.begin(), s.end(), ',');
    };
    REQUIRE(commas(row.substr(0, row.find('\n'))) == commas(header));
}

TEST_CASE("an uncounted breath total stays blank rather than becoming zero",
          "[server][survey]") {
    // Zero breaths in ninety seconds is a number, and a downstream mean would
    // happily average it in.
    TempDir tmp;
    std::string err;
    REQUIRE(store_survey(tmp.path, "trial_id: T\nbreaths_self_count: \n", err));
    REQUIRE(read_all(tmp.path / "surveys.csv").find(",0,") == std::string::npos);
}

TEST_CASE("a survey without a trial id is refused", "[server][survey]") {
    TempDir tmp;
    std::string err;
    REQUIRE_FALSE(store_survey(tmp.path, "relaxation: 5\n", err));
    REQUIRE(err.find("trial_id") != std::string::npos);
}

TEST_CASE("session rows append in schema order", "[server][survey]") {
    TempDir tmp;
    std::string err;
    REQUIRE(store_session(tmp.path,
        "session_id: P01_S01\nsubject_id: P01\ntrial_number: 1\n"
        "stim_mode: sweep\norder_seed: 84120\nround_count: 18\n", err));
    const std::string csv = read_all(tmp.path / "sessions.csv");
    REQUIRE(csv.find("session_id,subject_id,trial_number,date,stim_mode") == 0);
    REQUIRE(csv.find("P01_S01,P01,1,") != std::string::npos);
}

TEST_CASE("a suspect round is recorded as suspect", "[server][store]") {
    // The phone sets this when a round lost real time to a suspended tab or a
    // Bluetooth dropout. Before the column existed the flag was parsed and
    // then dropped, so such a round entered the dataset indistinguishable from
    // a clean one -- a partial recording that looks whole is worse than a
    // missing one, because nothing downstream has any reason to doubt it.
    TempDir tmp;
    RoundUpload r = sample_round();
    r.suspect = true;
    r.battery_pct = "87.4";
    r.temperature_c = "31";
    std::string err;
    REQUIRE(store_round(tmp.path, r, err));

    const std::string csv = read_all(tmp.path / "trials.csv");
    REQUIRE(csv.find("suspect,battery_pct,temperature_c") != std::string::npos);
    REQUIRE(csv.find(",1,87.4,31\n") != std::string::npos);
}

TEST_CASE("a clean round is not marked suspect", "[server][store]") {
    TempDir tmp;
    const RoundUpload r = sample_round();
    std::string err;
    REQUIRE(store_round(tmp.path, r, err));
    // Trailing empties are the honest answer when the headset reported no
    // telemetry, rather than a fabricated zero battery.
    REQUIRE(read_all(tmp.path / "trials.csv").find(",0,,\n") != std::string::npos);
}

TEST_CASE("telemetry survives the wire format", "[server][wire]") {
    std::string h = std::string(kHeaders) + "battery_pct: 92.5\ntemperature_c: 29\n";
    RoundUpload r;
    std::string err;
    REQUIRE(parse_round(envelope(h, "", ""), r, err));
    REQUIRE(r.battery_pct == "92.5");
    REQUIRE(r.temperature_c == "29");
}
