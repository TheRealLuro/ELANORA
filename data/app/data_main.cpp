// elanora_data -- the dataset browser and the evidence gate (Task 23).
//
// Three tabs, in the order the work actually happens:
//
//   Browse    what was recorded, and whether each signal was usable
//   Features  what the extractors made of it, per sensor and period
//   Evidence  whether any of it survives statistical scrutiny
//
// The Evidence tab is the one that matters. It carries a banner that says, in
// plain words, either that some outcomes show supported frequency effects or
// that none do -- and if none do, that the optimizer's recommendations are not
// meaningful yet. That sentence is the whole reason this application exists.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"

#include "elanora/data/dataset_builder.hpp"
#include "elanora/data/evidence.hpp"
#include "elanora/data/table.hpp"
#include "elanora/render.hpp"
#include "elanora/theme.hpp"
#include "elanora/ui_shell.hpp"

namespace fs = std::filesystem;

using namespace elanora;
using elanora::data::BuildReport;
using elanora::data::EvidenceOptions;
using elanora::data::OutcomeEvidence;
using elanora::data::Table;
using elanora::data::Verdict;

namespace {

ImVec4 v4(theme::Rgba c) { return ImVec4(c.r, c.g, c.b, c.a); }

// render.hpp's helpers live directly in namespace elanora, and it exposes no
// Rgba-to-ImU32 conversion, so this app does its own.
ImU32 u32(theme::Rgba c) { return ImGui::GetColorU32(v4(c)); }

theme::Rgba verdict_color(Verdict v) {
    switch (v) {
        case Verdict::Supported: return theme::kGood;
        case Verdict::Weak:      return theme::kWarn;
        default:                 return theme::kMuted;
    }
}

// Quality is shown as a coloured pill rather than a bare number, so a bad
// electrode is visible while scanning rather than only when read.
theme::Rgba quality_color(double q) {
    if (q >= 0.8) return theme::kGood;
    if (q >= 0.5) return theme::kWarn;
    return theme::kBad;
}

struct AppState {
    fs::path root = "data/datasets";

    Table sessions, trials, brain_features, heart_features, breath_features;
    std::vector<OutcomeEvidence> evidence;

    std::string status;
    bool busy = false;
    BuildReport last_build;
    bool has_build = false;

    int selected_trial = -1;
    int sensor_filter  = 0;   // 0 = all
    int period_filter  = 0;   // 0 = all

    EvidenceOptions ev_opt;
    int perm_choice = 1;      // index into kPermCounts
};

const int kPermCounts[] = {500, 2000, 5000};
const char* kPermLabels[] = {"500 (quick)", "2000", "5000 (full)"};

void reload(AppState& st) {
    st.trials         = Table::read_or_empty(st.root / "trials.csv");
    st.sessions       = Table::read_or_empty(st.root / "sessions.csv");
    st.brain_features = Table::read_or_empty(st.root / "features" / "brain_features.csv");
    st.heart_features = Table::read_or_empty(st.root / "features" / "heart_features.csv");
    st.breath_features = Table::read_or_empty(st.root / "features" / "breath_features.csv");
    st.evidence       = elanora::data::read_evidence(st.root);
}

// ---------------------------------------------------------------------------

void draw_header(AppState& st) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(theme::kPanel));
    ImGui::BeginChild("header", ImVec2(0, 74.0f), ImGuiChildFlags_AlwaysUseWindowPadding);

    eyebrow("DATASET");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme::kS2);
    ImGui::TextColored(v4(theme::kMuted), "%s", st.root.string().c_str());

    ImGui::SameLine();
    right_align(360.0f);

    if (st.busy) {
        ImGui::BeginDisabled();
        ImGui::Button("Working...", ImVec2(150.0f, 30.0f));
        ImGui::EndDisabled();
    } else if (ImGui::Button("Rebuild features", ImVec2(150.0f, 30.0f))) {
        st.busy = true;
        st.last_build = elanora::data::build_all(st.root);
        st.has_build = true;
        reload(st);
        st.busy = false;
        st.status = "rebuilt " + std::to_string(st.last_build.trials_processed) +
                    " trials, skipped " + std::to_string(st.last_build.trials_skipped);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(90.0f, 30.0f))) {
        reload(st);
        st.status = "reloaded from disk";
    }

    ImGui::Dummy(ImVec2(1, theme::kS1));
    const int n_sessions = static_cast<int>(st.trials.unique("session_id").size());
    ImGui::TextColored(v4(theme::kFaint), "%zu trials over %d sessions   |   %s",
                       st.trials.rows(), n_sessions,
                       st.status.empty() ? "ready" : st.status.c_str());

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_browse(AppState& st) {
    if (st.trials.empty()) {
        ImGui::Dummy(ImVec2(1, 40.0f));
        text_centered(ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 40.0f),
                              "No trials recorded yet.", theme::kMuted);
        return;
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("trials", 8, flags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("trial");
        ImGui::TableSetupColumn("session");
        ImGui::TableSetupColumn("subject");
        ImGui::TableSetupColumn("condition");
        ImGui::TableSetupColumn("Hz");
        ImGui::TableSetupColumn("EEG");
        ImGui::TableSetupColumn("heart");
        ImGui::TableSetupColumn("breath");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < st.trials.rows(); ++i) {
            const std::string trial_id = st.trials.get(i, "trial_id");
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (ImGui::Selectable(trial_id.c_str(), st.selected_trial == static_cast<int>(i),
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                st.selected_trial = static_cast<int>(i);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.trials.get(i, "session_id").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.trials.get(i, "subject_id").c_str());

            ImGui::TableNextColumn();
            const std::string cond = st.trials.get(i, "condition");
            ImGui::TextColored(v4(cond == "stim" ? theme::kAccent : theme::kMuted), "%s",
                               cond.c_str());

            ImGui::TableNextColumn();
            const double hz = st.trials.num(i, "frequency_hz");
            if (hz > 0.0) ImGui::Text("%.2f", hz);
            else ImGui::TextColored(v4(theme::kFaint), "-");

            // Per-signal quality, averaged over the three periods of this trial.
            // Shown separately because a trial with clean EEG and unusable
            // breathing still feeds the four brain models.
            auto mean_quality = [&](const Table& t, const char* col) {
                double sum = 0.0;
                int n = 0;
                for (std::size_t r = 0; r < t.rows(); ++r) {
                    if (t.get(r, "trial_id") != trial_id) continue;
                    sum += t.num(r, col);
                    ++n;
                }
                return n ? sum / n : -1.0;
            };

            for (auto [tbl, col] : {std::pair<const Table*, const char*>{&st.brain_features, "quality_eeg"},
                                    {&st.heart_features, "quality_heart"},
                                    {&st.breath_features, "confidence_breath"}}) {
                ImGui::TableNextColumn();
                const double q = mean_quality(*tbl, col);
                if (q < 0.0) ImGui::TextColored(v4(theme::kFaint), "-");
                else ImGui::TextColored(v4(quality_color(q)), "%.2f", q);
            }
        }
        ImGui::EndTable();
    }
}

void draw_features(AppState& st) {
    if (st.brain_features.empty()) {
        ImGui::Dummy(ImVec2(1, 40.0f));
        ImGui::TextColored(v4(theme::kMuted),
                           "No features yet. Use Rebuild features above.");
        return;
    }

    static const char* kSensors[] = {"all", "S1", "S2", "S3", "S4"};
    static const char* kPeriods[] = {"all", "baseline", "stimulus", "post"};

    ImGui::TextColored(v4(theme::kMuted), "sensor");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##sensor", &st.sensor_filter, kSensors, 5);
    ImGui::SameLine();
    ImGui::TextColored(v4(theme::kMuted), "period");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    ImGui::Combo("##period", &st.period_filter, kPeriods, 4);

    static bool show_relative = true;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme::kS3);
    ImGui::Checkbox("relative power", &show_relative);
    ImGui::SameLine();
    ImGui::TextColored(v4(theme::kFaint),
                       show_relative ? "(sums to 1; blind to amplitude)"
                                     : "(absolute; carries electrode impedance)");

    ImGui::Dummy(ImVec2(1, theme::kS2));

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("features", 10, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("trial");
        ImGui::TableSetupColumn("sensor");
        ImGui::TableSetupColumn("period");
        for (int b = 0; b < kBandCount; ++b) {
            ImGui::TableSetupColumn(band_name(static_cast<Band>(b)));
        }
        ImGui::TableSetupColumn("dominant");
        ImGui::TableSetupColumn("margin");
        ImGui::TableHeadersRow();

        for (std::size_t i = 0; i < st.brain_features.rows(); ++i) {
            if (st.sensor_filter > 0 &&
                st.brain_features.get(i, "sensor") != kSensors[st.sensor_filter]) continue;
            if (st.period_filter > 0 &&
                st.brain_features.get(i, "period") != kPeriods[st.period_filter]) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.brain_features.get(i, "trial_id").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.brain_features.get(i, "sensor").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.brain_features.get(i, "period").c_str());

            for (int b = 0; b < kBandCount; ++b) {
                ImGui::TableNextColumn();
                const std::string col = (show_relative ? "rel_" : "abs_") +
                                        std::string(band_name(static_cast<Band>(b)));
                const double v = st.brain_features.num(i, col);
                // The five band hues identify a band; they never encode value.
                ImGui::TextColored(v4(theme::band_color(b)),
                                   show_relative ? "%.3f" : "%.2f", v);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(st.brain_features.get(i, "dominant_band").c_str());
            ImGui::TableNextColumn();
            const double m = st.brain_features.num(i, "dominance_margin");
            // A small margin means the leader is not really leading, so it is
            // dimmed rather than presented as a clean classification.
            ImGui::TextColored(v4(m > 0.05 ? theme::kText : theme::kFaint), "%.3f", m);
        }
        ImGui::EndTable();
    }
}

void draw_evidence(AppState& st) {
    // ---- the banner -------------------------------------------------------
    const elanora::data::EvidenceSummary sum = elanora::data::summarize(st.evidence);

    const bool have_any = !st.evidence.empty();
    const theme::Rgba banner_col = !have_any        ? theme::kMuted
                                 : sum.any_supported() ? theme::kGood
                                                       : theme::kWarn;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(theme::kRaised));
    ImGui::BeginChild("banner", ImVec2(0, 118.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        // A severity stripe: the verdict reads at a glance, before any number.
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x - theme::kS3, p.y - theme::kS3),
            ImVec2(p.x - theme::kS3 + 4.0f, p.y + 118.0f), u32(banner_col), 2.0f);
    }
    eyebrow("EVIDENCE GATE");
    ImGui::Dummy(ImVec2(1, theme::kS1));

    if (!have_any) {
        ImGui::TextColored(v4(theme::kMuted),
                           "No analysis has been run yet. Rebuild features, then Analyze.");
    } else if (sum.any_supported()) {
        ImGui::TextColored(v4(theme::kGood), "%d of %d outcomes show supported frequency effects.",
                           sum.supported, static_cast<int>(st.evidence.size()));
        ImGui::TextColored(v4(theme::kFaint),
                           "Associational: these outcomes changed with frequency and differed "
                           "from control. That is not a causal claim.");
    } else {
        ImGui::TextColored(v4(theme::kWarn),
                           "No supported frequency effects. Optimizer recommendations are not "
                           "meaningful yet.");
        ImGui::TextColored(v4(theme::kFaint),
                           "This is a valid result, not a failure. %d outcomes were tested and "
                           "found nothing; %d could not be tested.", sum.none + sum.weak,
                           sum.untested);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(1, theme::kS2));

    // ---- controls ---------------------------------------------------------
    ImGui::TextColored(v4(theme::kMuted), "permutations");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    ImGui::Combo("##perm", &st.perm_choice, kPermLabels, 3);
    ImGui::SameLine();

    if (st.busy) {
        ImGui::BeginDisabled();
        ImGui::Button("Analyzing...", ImVec2(140.0f, 28.0f));
        ImGui::EndDisabled();
    } else if (ImGui::Button("Analyze", ImVec2(140.0f, 28.0f))) {
        st.busy = true;
        st.ev_opt.n_permutations = kPermCounts[st.perm_choice];
        st.evidence = elanora::data::analyze(st.root, st.ev_opt);
        elanora::data::write_evidence(st.root, st.evidence);
        st.busy = false;
        st.status = "analysis written to analysis/evidence.csv";
    }
    ImGui::SameLine();
    ImGui::TextColored(v4(theme::kFaint),
                       "FDR corrected across all %d outcomes", elanora::data::kOutcomeCount);

    ImGui::Dummy(ImVec2(1, theme::kS2));

    // ---- the table --------------------------------------------------------
    if (st.evidence.empty()) return;

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("evidence", 9, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("outcome");
        ImGui::TableSetupColumn("n");
        ImGui::TableSetupColumn("effect");
        ImGui::TableSetupColumn("p raw");
        ImGui::TableSetupColumn("p FDR");
        ImGui::TableSetupColumn("CV R2");
        ImGui::TableSetupColumn("null R2");
        ImGui::TableSetupColumn("p freq");
        ImGui::TableSetupColumn("verdict");
        ImGui::TableHeadersRow();

        for (const OutcomeEvidence& e : st.evidence) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.outcome.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", e.n_stim, e.n_control);

            if (!e.tested()) {
                // Untestable is not the same as tested-and-found-nothing, and
                // must never render as if it were.
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextColored(v4(theme::kFaint), "not tested");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.insufficient.c_str());
                continue;
            }

            ImGui::TableNextColumn();
            ImGui::Text("%+.2f", e.effect_stim_vs_control);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", e.p_raw);
            ImGui::TableNextColumn();
            ImGui::TextColored(v4(e.p_fdr < 0.05 ? theme::kGood : theme::kMuted), "%.4f",
                               e.p_fdr);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", e.cv_r2);
            ImGui::TableNextColumn();
            ImGui::TextColored(v4(theme::kMuted), "%.3f", e.cv_r2_null_mean);
            ImGui::TableNextColumn();
            ImGui::Text("%.4f", e.p_frequency);

            ImGui::TableNextColumn();
            status_pill(elanora::data::verdict_name(e.verdict), verdict_color(e.verdict));
        }
        ImGui::EndTable();
    }
}

}  // namespace

int main(int argc, char** argv) {
    AppState st;
    // Automation flags, so the app can be smoke-tested and captured without a
    // person driving it.
    int max_frames = -1;
    std::string shot;
    int shot_tab = -1;

    // Headless modes. Building a dataset and running the gate are batch jobs;
    // requiring a window for them would make this unusable over a remote shell
    // or from a script.
    bool headless_rebuild = false;
    bool headless_analyze = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) { max_frames = std::atoi(argv[++i]); continue; }
        if (a == "--screenshot" && i + 1 < argc) { shot = argv[++i]; continue; }
        if (a == "--tab" && i + 1 < argc) { shot_tab = std::atoi(argv[++i]); continue; }
        if (a == "--root" && i + 1 < argc) { st.root = argv[++i]; continue; }
        if (a == "--rebuild") { headless_rebuild = true; continue; }
        if (a == "--analyze") { headless_analyze = true; continue; }
    }

    if (headless_rebuild || headless_analyze) {
        if (headless_rebuild) {
            const BuildReport r = elanora::data::build_all(st.root);
            std::printf("features: %d processed, %d skipped, %d brain / %d heart / %d breath rows\n",
                        r.trials_processed, r.trials_skipped, r.brain_rows, r.heart_rows,
                        r.breath_rows);
            for (const std::string& w : r.warnings) std::printf("  warning: %s\n", w.c_str());
        }
        if (headless_analyze) {
            const std::vector<OutcomeEvidence> ev = elanora::data::analyze(st.root, st.ev_opt);
            elanora::data::write_evidence(st.root, ev);
            const elanora::data::EvidenceSummary sum = elanora::data::summarize(ev);
            std::printf("evidence: %d supported, %d weak, %d none, %d untested\n",
                        sum.supported, sum.weak, sum.none, sum.untested);
            if (!sum.any_supported()) {
                std::printf("No supported frequency effects. Optimizer recommendations are "
                            "not meaningful yet.\n");
            }
        }
        return 0;
    }

    UiShell shell("ELANORA - Data", 1500, 940);
    if (!shell.ok()) {
        std::fprintf(stderr, "could not open a window\n");
        return 1;
    }
    reload(st);

    int frames = 0;
    while (shell.begin_frame()) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        draw_header(st);
        ImGui::Dummy(ImVec2(1, theme::kS2));

        // --tab selects a tab for the first few frames so a capture can target
        // it; after that the selection is the user's again.
        auto tab_flags = [&](int want) {
            return (shot_tab == want && frames < 3) ? ImGuiTabItemFlags_SetSelected
                                                    : ImGuiTabItemFlags_None;
        };

        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Browse", nullptr, tab_flags(0)))   { draw_browse(st);   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Features", nullptr, tab_flags(1))) { draw_features(st); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Evidence", nullptr, tab_flags(2))) { draw_evidence(st); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::End();
        shell.end_frame();

        // Captured after the frame is presented, so the image is what a person
        // would actually see. save_screenshot reads only this app's own
        // framebuffer -- never the desktop.
        ++frames;
        if (!shot.empty() && frames == std::max(2, max_frames - 1)) {
            shell.save_screenshot(shot.c_str());
        }
        if (max_frames >= 0 && frames >= max_frames) break;
    }
    return 0;
}
