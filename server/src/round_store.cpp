#include "elanora/server/round_store.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>

#include "elanora/collector/schema.hpp"
#include "elanora/csv.hpp"

namespace elanora::server {

namespace fs = std::filesystem;
using collector::kEegHeader;
using collector::kImuHeader;
using collector::kMarkersHeader;
using collector::kPpgHeader;
using collector::kTrialsHeader;

namespace {

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += v[i];
    }
    return out;
}

// The first line, with any trailing CR removed. The phone emits \n endings, but
// a proxy or a manual re-upload can introduce \r\n, and a header that differs
// only by an invisible character would be rejected with a baffling message.
std::string first_line(const std::string& csv) {
    const std::size_t nl = csv.find('\n');
    std::string line = (nl == std::string::npos) ? csv : csv.substr(0, nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

bool header_ok(const std::string& csv, const std::vector<std::string>& expect,
               const char* what, std::string& err) {
    if (csv.empty()) return true;   // an absent stream is not an error
    const std::string got = first_line(csv);
    const std::string want = join(expect);
    if (got != want) {
        err = std::string(what) + " header is \"" + got + "\", expected \"" + want + "\"";
        return false;
    }
    return true;
}

// Rejects anything that could escape the dataset directory or collide with a
// path separator. These become directory and file names, and the value arrives
// over the network.
bool safe_id(const std::string& s, const char* what, std::string& err) {
    if (s.empty()) {
        err = std::string(what) + " is empty";
        return false;
    }
    for (const unsigned char c : s) {
        const bool ok = std::isalnum(c) || c == '_' || c == '-' || c == '.';
        if (!ok) {
            err = std::string(what) + " contains an illegal character";
            return false;
        }
    }
    if (s.find("..") != std::string::npos) {
        err = std::string(what) + " may not contain \"..\"";
        return false;
    }
    return true;
}

// Counts data rows, so the trials row records what actually landed rather than
// what the phone claimed it sent.
int data_rows(const std::string& csv) {
    if (csv.empty()) return 0;
    int n = 0;
    for (std::size_t i = 0; i < csv.size(); ++i) {
        if (csv[i] == '\n' && i + 1 < csv.size()) ++n;
    }
    return n;
}

bool write_file(const fs::path& p, const std::string& body, std::string& err) {
    if (body.empty()) return true;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "cannot write " + p.string();
        return false;
    }
    out << body;
    return true;
}

bool append_row(const fs::path& file, const std::vector<std::string>& header,
                const std::vector<std::string>& row, std::string& err) {
    std::error_code ec;
    const bool fresh = !fs::exists(file, ec);
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);

    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out) {
        err = "cannot write " + file.string();
        return false;
    }
    auto emit = [&out](const std::vector<std::string>& v) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (i) out.put(',');
            out << csv_escape(v[i]);
        }
        out.put('\n');
    };
    if (fresh) emit(header);
    emit(row);
    return true;
}

}  // namespace

bool store_round(const fs::path& root, const RoundUpload& r, std::string& err) {
    err.clear();

    if (!safe_id(r.session_id, "session_id", err)) return false;
    if (!safe_id(r.trial_id, "trial_id", err)) return false;

    if (!header_ok(r.eeg_csv, kEegHeader, "eeg", err)) return false;
    if (!header_ok(r.ppg_csv, kPpgHeader, "ppg", err)) return false;
    if (!header_ok(r.imu_csv, kImuHeader, "imu", err)) return false;
    if (!header_ok(r.markers_csv, kMarkersHeader, "markers", err)) return false;

    const fs::path dir = root / "raw" / r.session_id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = "cannot create " + dir.string();
        return false;
    }

    // Refuse rather than overwrite. A duplicate trial_id means the phone
    // retried a round the server already stored, and silently replacing it
    // would lose whichever copy was real.
    const fs::path eeg_path = dir / (r.trial_id + "_eeg.csv");
    if (fs::exists(eeg_path, ec)) {
        err = "round " + r.trial_id + " is already stored";
        return false;
    }

    if (!write_file(eeg_path, r.eeg_csv, err)) return false;
    if (!write_file(dir / (r.trial_id + "_ppg.csv"), r.ppg_csv, err)) return false;
    if (!write_file(dir / (r.trial_id + "_imu.csv"), r.imu_csv, err)) return false;
    if (!write_file(dir / (r.trial_id + "_markers.csv"), r.markers_csv, err)) return false;

    // started_at comes from the first marker, matching what TrialRecorder does.
    std::string started;
    {
        const std::size_t nl = r.markers_csv.find('\n');
        if (nl != std::string::npos) {
            const std::string row = r.markers_csv.substr(nl + 1);
            const std::size_t comma = row.find(',');
            if (comma != std::string::npos) started = row.substr(0, comma);
        }
    }

    return append_row(root / "trials.csv", kTrialsHeader,
                      {r.trial_id, r.session_id, r.subject_id,
                       std::to_string(r.round_index), r.condition,
                       fmt6(r.frequency_hz), fmt6(r.jitter_mean_hz), started,
                       std::to_string(data_rows(r.eeg_csv)),
                       std::to_string(data_rows(r.ppg_csv)),
                       std::to_string(data_rows(r.imu_csv)),
                       r.suspect ? "1" : "0", r.battery_pct, r.temperature_c},
                      err);
}


// ---------------------------------------------------------------------------
// Wire format
// ---------------------------------------------------------------------------
//
// Deliberately not JSON, which is what the plan first called for.
//
// A round carries roughly 400 KB of CSV across four streams, all of it full of
// newlines and commas. Putting that inside JSON means escaping every one of
// them and hand-writing an unescaper on this end, and a bug in that unescaper
// would corrupt sample data in ways that still parse as a valid CSV -- the
// exact failure this module exists to prevent.
//
// Length-prefixed sections have no escaping and therefore no escaping bugs:
//
//     session_id: P01_S01_20260903-120000
//     trial_id: P01_S01_20260903-120000_R01
//     condition: stim
//     round_index: 1
//     frequency_hz: 11.300000
//
//     @eeg 8342
//     timestamp,TP9,AF7,AF8,TP10
//     ...exactly 8342 bytes...
//     @ppg 1204
//     ...
//
// Headers end at the first blank line. Each section header is "@name <bytes>"
// followed by a newline and then exactly that many bytes, read by count.
namespace {

bool read_sections(const std::string& body, std::size_t pos, RoundUpload& out,
                   std::string& err) {
    while (pos < body.size()) {
        if (body[pos] != '@') {
            // Trailing whitespace after the last section is normal.
            while (pos < body.size() &&
                   (body[pos] == '\n' || body[pos] == '\r' || body[pos] == ' ')) ++pos;
            if (pos >= body.size()) return true;
            err = "expected a section marker at byte " + std::to_string(pos);
            return false;
        }
        const std::size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) { err = "truncated section header"; return false; }

        const std::string head = body.substr(pos + 1, nl - pos - 1);
        const std::size_t sp = head.find(' ');
        if (sp == std::string::npos) { err = "section header has no length"; return false; }

        const std::string name = head.substr(0, sp);
        std::size_t len = 0;
        try {
            len = static_cast<std::size_t>(std::stoull(head.substr(sp + 1)));
        } catch (const std::exception&) {
            err = "section " + name + " has an unreadable length";
            return false;
        }

        const std::size_t start = nl + 1;
        if (start + len > body.size()) {
            // The upload was cut short. Storing the prefix would look like a
            // successful round that happens to end early.
            err = "section " + name + " claims " + std::to_string(len) +
                  " bytes but only " + std::to_string(body.size() - start) + " remain";
            return false;
        }
        std::string payload = body.substr(start, len);

        if      (name == "eeg")     out.eeg_csv = std::move(payload);
        else if (name == "ppg")     out.ppg_csv = std::move(payload);
        else if (name == "imu")     out.imu_csv = std::move(payload);
        else if (name == "markers") out.markers_csv = std::move(payload);
        else { err = "unknown section \"" + name + "\""; return false; }

        pos = start + len;
    }
    return true;
}

double to_double(const std::string& s) {
    try { return std::stod(s); } catch (const std::exception&) { return 0.0; }
}

int to_int(const std::string& s) {
    try { return std::stoi(s); } catch (const std::exception&) { return 0; }
}

}  // namespace

bool parse_round(const std::string& body, RoundUpload& out, std::string& err) {
    err.clear();
    out = RoundUpload{};

    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = nl + 1;

        if (line.empty()) break;   // blank line ends the headers

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            err = "header line has no colon: \"" + line + "\"";
            return false;
        }
        const std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());

        if      (key == "session_id")     out.session_id = val;
        else if (key == "trial_id")       out.trial_id = val;
        else if (key == "subject_id")     out.subject_id = val;
        else if (key == "condition")      out.condition = val;
        else if (key == "round_index")    out.round_index = to_int(val);
        else if (key == "frequency_hz")   out.frequency_hz = to_double(val);
        else if (key == "jitter_mean_hz") out.jitter_mean_hz = to_double(val);
        else if (key == "suspect")        out.suspect = (val == "1" || val == "true");
        else if (key == "battery_pct")    out.battery_pct = val;
        else if (key == "temperature_c")  out.temperature_c = val;
        // An unknown header is ignored rather than rejected, so a newer phone
        // can add a field without breaking an older server.
    }

    if (!read_sections(body, pos, out, err)) return false;

    // The condition label is not optional. An unlabelled trial has no place in
    // this dataset -- the whole design rests on comparing stimulus to control.
    if (out.condition != "stim" && out.condition != "control_jitter" &&
        out.condition != "control_tone") {
        err = "condition must be stim, control_jitter or control_tone, got \"" +
              out.condition + "\"";
        return false;
    }
    return true;
}


// ---------------------------------------------------------------------------
// Surveys and session headers
// ---------------------------------------------------------------------------

namespace {

// The header block of the same envelope, as a map. Shared with parse_round so
// there is one definition of what a header line looks like.
std::map<std::string, std::string> parse_headers(const std::string& body) {
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        std::string line = body.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = nl + 1;
        if (line.empty()) break;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        out[line.substr(0, colon)] = val;
    }
    return out;
}

// Builds a row in the declared column order. A field the phone did not send
// becomes an empty cell rather than being skipped: a row with the wrong number
// of columns misaligns every value after the gap, and still parses.
bool store_keyed(const fs::path& root, const std::string& file,
                 const std::vector<std::string>& header, const std::string& body,
                 const char* id_field, std::string& err) {
    err.clear();
    const auto fields = parse_headers(body);

    const auto it = fields.find(id_field);
    if (it == fields.end() || it->second.empty()) {
        err = std::string("missing ") + id_field;
        return false;
    }
    if (!safe_id(it->second, id_field, err)) return false;

    std::vector<std::string> row;
    row.reserve(header.size());
    for (const std::string& col : header) {
        const auto f = fields.find(col);
        row.push_back(f == fields.end() ? std::string() : f->second);
    }
    return append_row(root / file, header, row, err);
}

}  // namespace

bool store_survey(const fs::path& root, const std::string& body, std::string& err) {
    return store_keyed(root, "surveys.csv", collector::kSurveysHeader, body,
                       "trial_id", err);
}

bool store_session(const fs::path& root, const std::string& body, std::string& err) {
    return store_keyed(root, "sessions.csv", collector::kSessionsHeader, body,
                       "session_id", err);
}

bool store_subject(const fs::path& root, const std::string& body, std::string& err) {
    return store_keyed(root, "subjects.csv", collector::kSubjectsHeader, body,
                       "subject_id", err);
}

}  // namespace elanora::server
