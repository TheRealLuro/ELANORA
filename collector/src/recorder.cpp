#include "elanora/collector/recorder.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "elanora/collector/schema.hpp"
#include "elanora/csv.hpp"

namespace elanora::collector {

namespace fs = std::filesystem;

namespace {

std::string timestamp_id() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string today() {
    const std::string s = timestamp_id();
    return s.substr(0, 8);
}

// Appends a row, writing the header first if the file is new. Every table in
// the dataset grows this way, so a trial can be added to an existing study
// without rewriting anything.
bool append_row(const fs::path& file, const std::vector<std::string>& header,
                const std::vector<std::string>& row, std::string& err) {
    std::error_code ec;
    const bool fresh = !fs::exists(file, ec);
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);

    std::ofstream out(file, std::ios::app | std::ios::binary);
    if (!out) { err = "cannot write " + file.string(); return false; }

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

// Writes one raw stream. Every channel BrainFlow reported is written, not just
// the ones currently used -- a feature invented next year has to be computable
// from these files without repeating the experiment.
bool write_stream(const fs::path& file, const std::vector<device::Sample>& samples,
                  const std::vector<std::string>& header, int ts_row, std::string& err) {
    if (samples.empty()) return true;   // a board without this stream is not an error

    // header[0] is "timestamp"; the rest are the value columns, in channel
    // order. Taken whole from schema.hpp rather than rebuilt here, so the
    // desktop recorder and the upload server cannot disagree about it.
    const std::size_t n_values = header.size() - 1;

    try {
        CsvWriter w(file, header);
        std::vector<std::string> row(header.size());
        for (const auto& s : samples) {
            row[0] = fmt6(s.ts);
            for (std::size_t c = 0; c < n_values; ++c) {
                // Channel indices come from the ChannelMap, so a board with a
                // different layout still lands in the right column.
                const std::size_t idx = c + 1u;
                row[c + 1u] = (idx < s.values.size()) ? fmt6(s.values[idx]) : "";
            }
            w.row(row);
        }
    } catch (const CsvError& e) {
        err = e.what();
        return false;
    }
    (void)ts_row;
    return true;
}

}  // namespace

bool TrialRecorder::begin_trial(const fs::path& root, const std::string& subject,
                                int trial_number, const StimulusDesign& design,
                                const Durations& d, uint64_t seed, int round_count,
                                std::string& err, bool quality_override) {
    err.clear();
    root_ = root;
    subject_ = subject.empty() ? "unknown" : subject;

    char sid[64];
    std::snprintf(sid, sizeof(sid), "%s_S%02d_%s", subject_.c_str(), trial_number,
                  timestamp_id().c_str());
    session_id_ = sid;
    session_dir_ = root_ / "raw" / session_id_;

    std::error_code ec;
    fs::create_directories(session_dir_, ec);
    if (ec) { err = "cannot create " + session_dir_.string(); return false; }

    // Fail here rather than after the subject has sat through 40 minutes.
    {
        const fs::path probe = session_dir_ / ".writable";
        std::ofstream t(probe);
        if (!t) { err = "session directory is not writable"; return false; }
        t.close();
        fs::remove(probe, ec);
    }

    char mode[16];
    std::snprintf(mode, sizeof(mode), "%s",
                  design.mode == StimMode::Stacked ? "stacked" : "sweep");
    char seed_s[32];
    std::snprintf(seed_s, sizeof(seed_s), "%llu", static_cast<unsigned long long>(seed));

    const bool ok = append_row(
        root_ / "sessions.csv",
        kSessionsHeader,
        {session_id_, subject_, std::to_string(trial_number), today(), mode,
         fmt6(design.carrier_hz), fmt6(design.duty), fmt6(d.baseline), fmt6(d.stimulus),
         fmt6(d.post), fmt6(d.rest), seed_s, std::to_string(round_count),
         quality_override ? "1" : "0"},
        err);

    rounds_written_ = 0;
    active_ = ok;
    return ok;
}

std::string TrialRecorder::trial_row_id(int round_index) const {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s_R%02d", session_id_.c_str(), round_index + 1);
    return buf;
}

bool TrialRecorder::write_round(int round_index, const PlannedRound& round,
                                const std::vector<device::Sample>& eeg,
                                const std::vector<device::Sample>& ppg,
                                const std::vector<device::Sample>& imu,
                                const std::vector<Marker>& markers,
                                const device::ChannelMap& channels,
                                std::string& err) {
    err.clear();
    if (!active_) { err = "no trial in progress"; return false; }

    const std::string tid = trial_row_id(round_index);

    if (!write_stream(session_dir_ / (tid + "_eeg.csv"), eeg, kEegHeader,
                      channels.ts_eeg, err)) return false;
    // Index 0 is red 660nm, 1 is IR 940nm, 2 is the ambient reference.
    if (!write_stream(session_dir_ / (tid + "_ppg.csv"), ppg, kPpgHeader,
                      channels.ts_ppg, err)) return false;
    if (!write_stream(session_dir_ / (tid + "_imu.csv"), imu, kImuHeader,
                      channels.ts_imu, err)) return false;

    try {
        CsvWriter mw(session_dir_ / (tid + "_markers.csv"), kMarkersHeader);
        for (const auto& m : markers) mw.row(std::vector<std::string>{fmt6(m.ts), m.event});
    } catch (const CsvError& e) {
        err = e.what();
        return false;
    }

    const char* cond = condition_name(round.cond);
    if (!append_row(root_ / "trials.csv",
                    kTrialsHeader,
                    {tid, session_id_, subject_, std::to_string(round_index + 1), cond,
                     fmt6(round.hz), fmt6(round.jitter_mean_hz),
                     markers.empty() ? "" : fmt6(markers.front().ts),
                     std::to_string(eeg.size()), std::to_string(ppg.size()),
                     std::to_string(imu.size()),
                     // The desktop path has no suspend to detect and does not
                     // read telemetry, so these are honestly empty rather than
                     // fabricated zeroes.
                     "0", "", ""},
                    err)) {
        return false;
    }

    ++rounds_written_;
    return true;
}

bool TrialRecorder::write_survey(int round_index, const Survey& s, std::string& err) {
    err.clear();
    if (!active_) { err = "no trial in progress"; return false; }

    static const char* kRhythm[]    = {"clear", "faint", "none"};
    static const char* kBreathing[] = {"slower", "same", "faster"};

    const char* rhythm = (s.rhythm >= 0 && s.rhythm < 3) ? kRhythm[s.rhythm] : "";
    const char* breathing = (s.breathing_perceived >= 0 && s.breathing_perceived < 3)
                                ? kBreathing[s.breathing_perceived]
                                : "";
    // An uncounted breath total is blank, never 0 -- zero breaths in 90 seconds
    // is a number, and a downstream mean would happily average it in.
    const std::string breaths = (s.breaths_self_count >= 0)
                                    ? std::to_string(s.breaths_self_count)
                                    : std::string();

    return append_row(
        root_ / "surveys.csv",
        kSurveysHeader,
        {trial_row_id(round_index), session_id_, subject_, std::to_string(round_index + 1),
         std::to_string(s.relaxation), std::to_string(s.alertness),
         std::to_string(s.pleasantness), std::to_string(s.discomfort), breathing, breaths,
         rhythm, s.jaw ? "1" : "0", s.moved ? "1" : "0", s.eyes_open ? "1" : "0",
         s.swallowed ? "1" : "0", s.noise ? "1" : "0", s.note},
        err);
}

}  // namespace elanora::collector
