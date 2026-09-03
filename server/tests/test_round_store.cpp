#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(trials.find(",3,1,1\n") != std::string::npos);
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
