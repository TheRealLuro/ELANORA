#include "elanora/data/dataset_builder.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include "elanora/csv.hpp"
#include "elanora/data/breath_features.hpp"
#include "elanora/data/eeg_features.hpp"
#include "elanora/data/heart_features.hpp"
#include "elanora/data/qc.hpp"
#include "elanora/data/table.hpp"
#include "elanora/types.hpp"

namespace elanora::data {

namespace fs = std::filesystem;

namespace {

const char* kElectrodeCols[kSensorCount] = {"TP9", "AF7", "AF8", "TP10"};
const char* kImuCols[kImuAxisCount] = {"ax", "ay", "az", "gx", "gy", "gz"};

// One period's boundaries, taken from markers.csv rather than reconstructed
// from durations. Durations drift against the sample clock; the markers were
// stamped from it.
struct PeriodBounds {
    double t0 = 0.0;
    double t1 = 0.0;
    bool valid() const { return t1 > t0; }
};

struct TrialWindows {
    std::array<PeriodBounds, kPeriodCount> period;
    bool complete() const {
        return period[0].valid() && period[1].valid() && period[2].valid();
    }
};

TrialWindows windows_from_markers(const Table& markers) {
    TrialWindows w;
    double baseline = 0, stimulus = 0, post = 0, end = 0;
    bool hb = false, hs = false, hp = false, he = false;

    for (std::size_t i = 0; i < markers.rows(); ++i) {
        const std::string ev = markers.get(i, "event");
        const double ts = markers.num(i, "timestamp");
        if (ev == "baseline_start")      { baseline = ts; hb = true; }
        else if (ev == "stimulus_start") { stimulus = ts; hs = true; }
        else if (ev == "post_start")     { post = ts;     hp = true; }
        else if (ev == "trial_end")      { end = ts;      he = true; }
    }
    if (!(hb && hs && hp && he)) return w;

    w.period[index_of(Period::Baseline)] = {baseline, stimulus};
    w.period[index_of(Period::Stimulus)] = {stimulus, post};
    w.period[index_of(Period::Post)]     = {post, end};
    return w;
}

// Pulls one column out of a raw stream file, restricted to a time window.
// Missing samples stay NaN so the feature layer can account for them; dropping
// them would silently change the effective sample rate.
std::vector<double> slice_column(const Table& t, const std::string& col,
                                 const PeriodBounds& b) {
    std::vector<double> out;
    const int c = t.column(col);
    const int tc = t.column("timestamp");
    if (c < 0 || tc < 0) return out;

    for (std::size_t i = 0; i < t.rows(); ++i) {
        const double ts = t.num(i, "timestamp");
        if (ts < b.t0 || ts >= b.t1) continue;
        out.push_back(t.is_blank(i, col) ? std::numeric_limits<double>::quiet_NaN()
                                         : t.num(i, col));
    }
    return out;
}

// Sample rate inferred from the timestamps actually present, not assumed from
// the datasheet. A stream that dropped half its packets has a real rate of half
// nominal, and using the nominal value would misplace every frequency.
int infer_rate(const Table& t, int fallback) {
    const int tc = t.column("timestamp");
    if (tc < 0 || t.rows() < 8) return fallback;
    const double first = t.num(0, "timestamp");
    const double last  = t.num(t.rows() - 1, "timestamp");
    const double span  = last - first;
    if (span <= 0.0) return fallback;
    const double rate = static_cast<double>(t.rows() - 1) / span;
    if (rate < 1.0 || rate > 2000.0) return fallback;
    return static_cast<int>(std::lround(rate));
}

std::string band_col(const char* prefix, Band b) {
    return std::string(prefix) + band_name(b);
}

// The frequency a row is "about". Controls have no stimulus frequency, but a
// jitter control has a mean rate, and that is what it must be compared against.
double effective_frequency(const std::string& condition, double hz, double jitter) {
    if (condition == "control_tone") return 0.0;
    if (condition == "control_jitter") return (jitter > 0.0) ? jitter : hz;
    return hz;
}

std::string fmt_or_blank(double v) {
    return std::isfinite(v) ? fmt6(v) : std::string();
}

}  // namespace

std::vector<double> clr(const std::vector<double>& composition) {
    std::vector<double> out(composition.size(), 0.0);
    if (composition.empty()) return out;

    double sum_log = 0.0;
    for (double v : composition) sum_log += std::log(std::max(v, 0.0) + kLogEpsilon);
    const double mean_log = sum_log / static_cast<double>(composition.size());

    for (std::size_t i = 0; i < composition.size(); ++i) {
        out[i] = std::log(std::max(composition[i], 0.0) + kLogEpsilon) - mean_log;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stage 1: raw -> long feature tables
// ---------------------------------------------------------------------------

BuildReport build_features(const fs::path& root) {
    BuildReport rep;

    const Table trials = Table::read_or_empty(root / "trials.csv");
    if (trials.empty()) {
        rep.warnings.push_back("no trials.csv under " + root.string());
        return rep;
    }

    std::vector<std::string> brain_header = {
        "subject_id", "session_id", "trial_id", "condition", "frequency_hz",
        "sensor", "electrode", "period"};
    for (int b = 0; b < kBandCount; ++b) {
        brain_header.push_back(band_col("abs_", static_cast<Band>(b)));
    }
    for (int b = 0; b < kBandCount; ++b) {
        brain_header.push_back(band_col("rel_", static_cast<Band>(b)));
    }
    brain_header.insert(brain_header.end(),
                        {"total_power", "dominant_band", "dominance_margin",
                         "codominant_bands", "quality_eeg"});

    Table brain(brain_header);
    Table heart({"subject_id", "session_id", "trial_id", "condition", "frequency_hz",
                 "period", "bpm", "rmssd", "n_beats", "quality_heart"});
    Table breath({"subject_id", "session_id", "trial_id", "condition", "frequency_hz",
                  "period", "breaths_per_minute", "bpm_spectral", "n_cycles",
                  "axis_used", "motion_energy", "confidence_breath"});

    for (std::size_t i = 0; i < trials.rows(); ++i) {
        ++rep.trials_seen;

        const std::string trial_id  = trials.get(i, "trial_id");
        const std::string sess_id   = trials.get(i, "session_id");
        const std::string subj_id   = trials.get(i, "subject_id");
        const std::string condition = trials.get(i, "condition");
        const double freq = effective_frequency(condition, trials.num(i, "frequency_hz"),
                                                trials.num(i, "jitter_mean_hz"));

        const fs::path dir = root / "raw" / sess_id;
        const Table markers = Table::read_or_empty(dir / (trial_id + "_markers.csv"));
        if (markers.empty()) {
            rep.warnings.push_back(trial_id + ": no markers file, skipped");
            ++rep.trials_skipped;
            continue;
        }

        const TrialWindows win = windows_from_markers(markers);
        if (!win.complete()) {
            // An aborted round has a baseline marker and nothing after it.
            // Skipping loudly is right; a partial trial would contribute a
            // baseline with no response and bias every delta toward zero.
            rep.warnings.push_back(trial_id + ": incomplete markers, skipped");
            ++rep.trials_skipped;
            continue;
        }

        const Table eeg_t = Table::read_or_empty(dir / (trial_id + "_eeg.csv"));
        const Table ppg_t = Table::read_or_empty(dir / (trial_id + "_ppg.csv"));
        const Table imu_t = Table::read_or_empty(dir / (trial_id + "_imu.csv"));

        if (eeg_t.empty()) {
            rep.warnings.push_back(trial_id + ": no EEG file, skipped");
            ++rep.trials_skipped;
            continue;
        }

        const int sr_eeg = infer_rate(eeg_t, 256);
        const int sr_ppg = infer_rate(ppg_t, 64);
        const int sr_imu = infer_rate(imu_t, 52);

        for (int p = 0; p < kPeriodCount; ++p) {
            const Period period = static_cast<Period>(p);
            const PeriodBounds& b = win.period[static_cast<std::size_t>(p)];

            std::vector<std::vector<double>> eeg_ch;
            for (int s = 0; s < kSensorCount; ++s) {
                eeg_ch.push_back(slice_column(eeg_t, kElectrodeCols[s], b));
            }
            const std::vector<double> ppg_ir = slice_column(ppg_t, "ppg_ir", b);

            std::array<std::vector<double>, kImuAxisCount> imu;
            for (int a = 0; a < kImuAxisCount; ++a) {
                imu[static_cast<std::size_t>(a)] = slice_column(imu_t, kImuCols[a], b);
            }

            const SignalQc qc = assess_period(eeg_ch, ppg_ir, imu, sr_eeg, sr_ppg, sr_imu);

            for (int s = 0; s < kSensorCount; ++s) {
                const SensorId sid = static_cast<SensorId>(s);
                const BandPowers bp = band_powers(eeg_ch[static_cast<std::size_t>(s)], sr_eeg);
                const Dominance dom = assess_dominance(bp);

                std::string codom;
                for (std::size_t k = 0; k < dom.codominant.size(); ++k) {
                    if (k) codom += "|";   // not a comma: this is one CSV field
                    codom += band_name(dom.codominant[k]);
                }

                std::vector<std::string> row = {
                    subj_id, sess_id, trial_id, condition, fmt6(freq),
                    sensor_name(sid), electrode_name(sid), period_name(period)};
                for (int k = 0; k < kBandCount; ++k) {
                    row.push_back(fmt6(bp.abs[static_cast<std::size_t>(k)]));
                }
                for (int k = 0; k < kBandCount; ++k) {
                    row.push_back(fmt6(bp.rel[static_cast<std::size_t>(k)]));
                }
                row.push_back(fmt6(bp.total));
                row.push_back(band_name(dom.dominant));
                row.push_back(fmt6(dom.margin));
                row.push_back(codom);
                row.push_back(fmt6(qc.eeg[static_cast<std::size_t>(s)]));
                brain.add_row(std::move(row));
                ++rep.brain_rows;
            }

            const HeartFeatures hf = heart_from_ppg(ppg_ir, sr_ppg);
            heart.add_row({subj_id, sess_id, trial_id, condition, fmt6(freq),
                           period_name(period), fmt6(hf.bpm), fmt6(hf.rmssd),
                           std::to_string(hf.n_beats), fmt6(hf.quality)});
            ++rep.heart_rows;

            const BreathFeatures bf = breath_from_imu(imu, sr_imu, qc.motion_energy);
            breath.add_row({subj_id, sess_id, trial_id, condition, fmt6(freq),
                            period_name(period), fmt6(bf.breaths_per_minute),
                            fmt6(bf.bpm_spectral), std::to_string(bf.n_cycles),
                            bf.axis_used, fmt6(qc.motion_energy), fmt6(bf.confidence)});
            ++rep.breath_rows;
        }
        ++rep.trials_processed;
    }

    brain.write(root / "features" / "brain_features.csv");
    heart.write(root / "features" / "heart_features.csv");
    breath.write(root / "features" / "breath_features.csv");
    return rep;
}

// ---------------------------------------------------------------------------
// Stage 2: long -> wide ML tables
// ---------------------------------------------------------------------------

namespace {

// One trial-and-sensor's three periods, gathered before any response is formed.
struct BrainCell {
    bool present[kPeriodCount] = {false, false, false};
    std::array<std::array<double, kBandCount>, kPeriodCount> logabs{};
    std::array<std::array<double, kBandCount>, kPeriodCount> clr_v{};
    std::array<double, kPeriodCount> quality{};
    std::string subject, session, condition, sensor, electrode;
    double frequency = 0.0;
};

struct ScalarCell {
    bool present[kPeriodCount] = {false, false, false};
    std::array<double, kPeriodCount> value{};
    std::array<double, kPeriodCount> extra{};    // rmssd / spectral rate
    std::array<double, kPeriodCount> quality{};
    std::string subject, session, condition;
    double frequency = 0.0;
};

int period_index(const std::string& name) {
    for (int p = 0; p < kPeriodCount; ++p) {
        if (name == period_name(static_cast<Period>(p))) return p;
    }
    return -1;
}

void push_triplet(std::vector<std::string>& row, const char* stem,
                  const std::array<double, kPeriodCount>& v,
                  const bool (&present)[kPeriodCount]) {
    (void)stem;
    for (int p = 0; p < kPeriodCount; ++p) {
        row.push_back(present[p] ? fmt6(v[static_cast<std::size_t>(p)]) : std::string());
    }
}

}  // namespace

BuildReport build_ml_datasets(const fs::path& root) {
    BuildReport rep;

    const Table brain_f  = Table::read_or_empty(root / "features" / "brain_features.csv");
    const Table heart_f  = Table::read_or_empty(root / "features" / "heart_features.csv");
    const Table breath_f = Table::read_or_empty(root / "features" / "breath_features.csv");

    if (brain_f.empty()) {
        rep.warnings.push_back("no brain_features.csv; run build_features first");
        return rep;
    }

    // ---- brain ------------------------------------------------------------
    std::map<std::string, BrainCell> cells;
    for (std::size_t i = 0; i < brain_f.rows(); ++i) {
        const int p = period_index(brain_f.get(i, "period"));
        if (p < 0) continue;

        const std::string key = brain_f.get(i, "trial_id") + "|" + brain_f.get(i, "sensor");
        BrainCell& c = cells[key];
        c.subject   = brain_f.get(i, "subject_id");
        c.session   = brain_f.get(i, "session_id");
        c.condition = brain_f.get(i, "condition");
        c.sensor    = brain_f.get(i, "sensor");
        c.electrode = brain_f.get(i, "electrode");
        c.frequency = brain_f.num(i, "frequency_hz");

        std::vector<double> rel(kBandCount, 0.0);
        for (int b = 0; b < kBandCount; ++b) {
            const Band band = static_cast<Band>(b);
            const double a = brain_f.num(i, band_col("abs_", band));
            rel[static_cast<std::size_t>(b)] = brain_f.num(i, band_col("rel_", band));
            // Absolute power is right-skewed across orders of magnitude, so it
            // is logged before anything is subtracted. A difference of logs is
            // a log-ratio: doubling is +0.693 whatever the starting level, and
            // that is the only scale on which a delta means the same thing for
            // a strong and a weak signal.
            c.logabs[static_cast<std::size_t>(p)][static_cast<std::size_t>(b)] =
                std::log(std::max(a, 0.0) + kLogEpsilon);
        }
        const std::vector<double> cv = clr(rel);
        for (int b = 0; b < kBandCount; ++b) {
            c.clr_v[static_cast<std::size_t>(p)][static_cast<std::size_t>(b)] =
                cv[static_cast<std::size_t>(b)];
        }
        c.quality[static_cast<std::size_t>(p)] = brain_f.num(i, "quality_eeg");
        c.present[p] = true;
    }

    std::vector<std::string> bh = {"subject_id", "session_id", "trial_id", "condition",
                                   "frequency_hz", "log2_freq", "sensor", "electrode"};
    for (const char* rep_name : {"logabs", "clr"}) {
        for (const char* stage : {"baseline", "stimulus", "post", "d", "post_d", "recovery"}) {
            for (int b = 0; b < kBandCount; ++b) {
                bh.push_back(std::string(stage) + "_" + rep_name + "_" +
                             band_name(static_cast<Band>(b)));
            }
        }
    }
    bh.push_back("quality_eeg");
    Table brain_ml(bh);

    const int kB = index_of(Period::Baseline);
    const int kS = index_of(Period::Stimulus);
    const int kP = index_of(Period::Post);

    for (const auto& [key, c] : cells) {
        if (!(c.present[kB] && c.present[kS] && c.present[kP])) {
            rep.warnings.push_back(key + ": missing a period, excluded from ML table");
            continue;
        }
        const std::string trial_id = key.substr(0, key.find('|'));

        // Models consume log2(frequency), never raw Hz: the design is
        // log-spaced, and a linear feature would let 32 and 45 Hz dominate the
        // fit while everything under 4 Hz became indistinguishable from zero.
        const std::string log2f = (c.frequency > 0.0)
                                      ? fmt6(std::log2(c.frequency))
                                      : std::string();

        std::vector<std::string> row = {c.subject, c.session, trial_id, c.condition,
                                        fmt6(c.frequency), log2f, c.sensor, c.electrode};

        for (int r = 0; r < 2; ++r) {
            const auto& v = (r == 0) ? c.logabs : c.clr_v;
            for (int b = 0; b < kBandCount; ++b) {
                row.push_back(fmt6(v[static_cast<std::size_t>(kB)][static_cast<std::size_t>(b)]));
            }
            for (int b = 0; b < kBandCount; ++b) {
                row.push_back(fmt6(v[static_cast<std::size_t>(kS)][static_cast<std::size_t>(b)]));
            }
            for (int b = 0; b < kBandCount; ++b) {
                row.push_back(fmt6(v[static_cast<std::size_t>(kP)][static_cast<std::size_t>(b)]));
            }
            // The three response types, per the schema: immediate, persistent,
            // and the return toward baseline.
            for (int b = 0; b < kBandCount; ++b) {
                const auto bi = static_cast<std::size_t>(b);
                row.push_back(fmt6(v[static_cast<std::size_t>(kS)][bi] -
                                   v[static_cast<std::size_t>(kB)][bi]));
            }
            for (int b = 0; b < kBandCount; ++b) {
                const auto bi = static_cast<std::size_t>(b);
                row.push_back(fmt6(v[static_cast<std::size_t>(kP)][bi] -
                                   v[static_cast<std::size_t>(kB)][bi]));
            }
            for (int b = 0; b < kBandCount; ++b) {
                const auto bi = static_cast<std::size_t>(b);
                row.push_back(fmt6(v[static_cast<std::size_t>(kP)][bi] -
                                   v[static_cast<std::size_t>(kS)][bi]));
            }
        }
        // The worst of the three periods: a response is only as trustworthy as
        // the weakest measurement it was derived from.
        row.push_back(fmt6(std::min({c.quality[static_cast<std::size_t>(kB)],
                                     c.quality[static_cast<std::size_t>(kS)],
                                     c.quality[static_cast<std::size_t>(kP)]})));
        brain_ml.add_row(std::move(row));
        ++rep.brain_rows;
    }
    brain_ml.write(root / "ml" / "brain_dataset.csv");

    // ---- heart and breathing ---------------------------------------------
    auto gather_scalar = [](const Table& t, const char* value_col, const char* extra_col,
                            const char* quality_col) {
        std::map<std::string, ScalarCell> out;
        for (std::size_t i = 0; i < t.rows(); ++i) {
            const int p = period_index(t.get(i, "period"));
            if (p < 0) continue;
            ScalarCell& c = out[t.get(i, "trial_id")];
            c.subject   = t.get(i, "subject_id");
            c.session   = t.get(i, "session_id");
            c.condition = t.get(i, "condition");
            c.frequency = t.num(i, "frequency_hz");
            c.value[static_cast<std::size_t>(p)]   = t.num(i, value_col);
            c.extra[static_cast<std::size_t>(p)]   = t.num(i, extra_col);
            c.quality[static_cast<std::size_t>(p)] = t.num(i, quality_col);
            c.present[p] = true;
        }
        return out;
    };

    auto write_scalar = [&](const std::map<std::string, ScalarCell>& cells_in,
                            const char* stem, const char* extra_stem,
                            const char* quality_name, const fs::path& out_path) {
        std::vector<std::string> h = {"subject_id", "session_id", "trial_id", "condition",
                                      "frequency_hz", "log2_freq"};
        for (const char* stage : {"baseline", "stimulus", "post", "d", "post_d", "recovery"}) {
            h.push_back(std::string(stage) + "_" + stem);
        }
        h.push_back(std::string("baseline_") + extra_stem);
        h.push_back(std::string("d_") + extra_stem);
        h.push_back(quality_name);
        Table out(h);

        int written = 0;
        for (const auto& [trial_id, c] : cells_in) {
            if (!(c.present[kB] && c.present[kS] && c.present[kP])) continue;
            const auto b = static_cast<std::size_t>(kB);
            const auto s = static_cast<std::size_t>(kS);
            const auto p = static_cast<std::size_t>(kP);

            std::vector<std::string> row = {
                c.subject, c.session, trial_id, c.condition, fmt6(c.frequency),
                (c.frequency > 0.0) ? fmt6(std::log2(c.frequency)) : std::string()};

            row.push_back(fmt_or_blank(c.value[b]));
            row.push_back(fmt_or_blank(c.value[s]));
            row.push_back(fmt_or_blank(c.value[p]));
            row.push_back(fmt_or_blank(c.value[s] - c.value[b]));
            row.push_back(fmt_or_blank(c.value[p] - c.value[b]));
            row.push_back(fmt_or_blank(c.value[p] - c.value[s]));
            row.push_back(fmt_or_blank(c.extra[b]));
            row.push_back(fmt_or_blank(c.extra[s] - c.extra[b]));
            row.push_back(fmt6(std::min({c.quality[b], c.quality[s], c.quality[p]})));
            out.add_row(std::move(row));
            ++written;
        }
        out.write(out_path);
        return written;
    };

    rep.heart_rows = write_scalar(
        gather_scalar(heart_f, "bpm", "rmssd", "quality_heart"), "bpm", "rmssd",
        "quality_heart", root / "ml" / "heart_dataset.csv");

    rep.breath_rows = write_scalar(
        gather_scalar(breath_f, "breaths_per_minute", "bpm_spectral", "confidence_breath"),
        "breathing", "breathing_spectral", "confidence_breath",
        root / "ml" / "breath_dataset.csv");

    rep.trials_processed = static_cast<int>(cells.size()) / kSensorCount;
    return rep;
}

BuildReport build_all(const fs::path& root) {
    BuildReport a = build_features(root);
    const BuildReport b = build_ml_datasets(root);
    a.warnings.insert(a.warnings.end(), b.warnings.begin(), b.warnings.end());
    return a;
}

}  // namespace elanora::data
