// elanora_models -- train the six models, predict forward, optimise inverse.
//
// Every tab carries the evidence verdict. That is deliberate: a recommendation
// is only as good as the evidence that frequency does anything at all, and
// burying that on another screen would let someone act on a number without it.
//
// All result wording here is associational. "Associated with a predicted
// increase", never "causes". Control comparison and repetition strengthen an
// inference; they do not license the stronger word.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"

#include "elanora/csv.hpp"
#include "elanora/data/evidence.hpp"
#include "elanora/models/model_registry.hpp"
#include "elanora/models/optimizer.hpp"
#include "elanora/render.hpp"
#include "elanora/theme.hpp"
#include "elanora/ui_shell.hpp"

namespace fs = std::filesystem;

using namespace elanora;
using namespace elanora::models;
using elanora::data::OutcomeEvidence;
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

struct AppState {
    fs::path datasets = "data/datasets";
    fs::path trained  = "models/trained";

    ModelRegistry reg;
    std::vector<OutcomeEvidence> evidence;
    BaselineState baseline;

    std::string status;
    bool trained_now = false;

    // Forward tab
    float forward_log2f = 3.32f;   // 10 Hz
    // Which outcome the response curve is showing. The bars answer "what
    // happens at this frequency"; the curve answers "how does it vary across
    // frequency", which is the question the models were actually built for.
    ModelId sel_model  = ModelId::BrainS1;
    int     sel_output = 2;        // alpha

    // Inverse tab
    DesiredState desired;
    OptimizerResult result;
    bool have_result = false;
};

const OutcomeEvidence* evidence_for(const AppState& st, ModelId m, int k) {
    const std::string key = evidence_key(m, k, st.reg.options().response);
    for (const OutcomeEvidence& e : st.evidence) {
        if (e.outcome == key) return &e;
    }
    return nullptr;
}

Verdict verdict_for(const AppState& st, ModelId m, int k) {
    const OutcomeEvidence* e = evidence_for(st, m, k);
    return e ? e->verdict : Verdict::NoEvidence;
}

// ---------------------------------------------------------------------------

void draw_evidence_banner(const AppState& st) {
    const elanora::data::EvidenceSummary sum = elanora::data::summarize(st.evidence);
    const bool any = sum.any_supported();
    const theme::Rgba col = st.evidence.empty() ? theme::kMuted
                          : any                 ? theme::kGood
                                                : theme::kWarn;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(theme::kRaised));
    ImGui::BeginChild("evbanner", ImVec2(0, 78.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p.x - theme::kS3, p.y - theme::kS3),
            ImVec2(p.x - theme::kS3 + 4.0f, p.y + 78.0f), u32(col), 2.0f);
    }
    if (st.evidence.empty()) {
        ImGui::TextColored(v4(theme::kMuted),
                           "No evidence file. Run the Evidence tab in elanora_data first -- "
                           "recommendations are gated on it.");
    } else if (any) {
        ImGui::TextColored(v4(theme::kGood), "%d supported outcomes.", sum.supported);
        ImGui::TextColored(v4(theme::kFaint),
                           "Goals on unsupported outcomes are refused, not merely flagged.");
    } else {
        ImGui::TextColored(v4(theme::kWarn),
                           "No supported frequency effects. Every goal will be refused.");
        ImGui::TextColored(v4(theme::kFaint),
                           "That is the design working: there is nothing here to optimise over.");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_train(AppState& st) {
    if (ImGui::Button("Train all", ImVec2(140.0f, 32.0f))) {
        st.reg.train_all(st.datasets);
        st.baseline = BaselineState::from_registry(st.reg);
        st.trained_now = true;
        st.status = st.reg.any_trained() ? "training complete" : "nothing could be trained";
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(90.0f, 32.0f))) {
        std::string err;
        st.status = st.reg.save(st.trained, err) ? "saved to " + st.trained.string()
                                                 : "save failed: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load", ImVec2(90.0f, 32.0f))) {
        std::string err;
        if (st.reg.load(st.trained, err)) {
            st.baseline = BaselineState::from_registry(st.reg);
            st.status = "loaded from " + st.trained.string();
        } else {
            st.status = "load failed: " + err;
        }
    }
    ImGui::SameLine();
    ImGui::TextColored(v4(theme::kFaint), "%s", st.status.c_str());

    const auto [lo, hi] = st.reg.trained_freq_range();
    ImGui::Dummy(ImVec2(1, theme::kS2));
    if (lo > 0.0) {
        ImGui::TextColored(v4(theme::kMuted),
                           "trained range %.2f - %.1f Hz over %zu presented frequencies",
                           lo, hi, st.reg.trained_frequencies().size());
    }

    ImGui::Dummy(ImVec2(1, theme::kS2));

    // All six cards scroll together. Each gets an explicit height: a card sized
    // (0,0) fills the remaining space, so the first model would consume the
    // whole region and the other five would never be reachable.
    ImGui::BeginChild("modellist", ImVec2(0, 0), ImGuiChildFlags_None);

    for (int i = 0; i < kModelCount; ++i) {
        const ModelId m = static_cast<ModelId>(i);
        const int n_out = outputs_for(m);

        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const float card_h = st.reg.trained(m)
                                 ? theme::kS4 * 2.0f + row_h * (n_out + 2.3f)
                                 : theme::kS4 * 2.0f + row_h * 2.2f;

        ImGui::PushID(i);
        if (begin_card("model", ImVec2(0, card_h))) {
            eyebrow(model_id_name(m));
            ImGui::SameLine();
            right_align(220.0f);
            if (st.reg.trained(m)) {
                status_pill("trained", theme::kGood);
                ImGui::SameLine();
                ImGui::TextColored(v4(theme::kMuted), "n = %d", st.reg.n_samples(m));
            } else {
                status_pill("untrained", theme::kWarn);
            }

            if (!st.reg.trained(m)) {
                ImGui::TextColored(v4(theme::kFaint), "%s",
                                   st.reg.untrained_reason(m).empty()
                                       ? "not trained yet"
                                       : st.reg.untrained_reason(m).c_str());
            } else {
                const ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("outs", 6, f)) {
                    ImGui::TableSetupColumn("output");
                    ImGui::TableSetupColumn("model");
                    ImGui::TableSetupColumn("session R2");
                    ImGui::TableSetupColumn("subject R2");
                    ImGui::TableSetupColumn("RMSE");
                    ImGui::TableSetupColumn("evidence");
                    ImGui::TableHeadersRow();

                    for (int k = 0; k < n_out; ++k) {
                        const TrainedOutput& o = st.reg.output(m, k);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(output_name(m, k).c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(model_kind_name(o.kind));

                        ImGui::TableNextColumn();
                        // A model that does not beat predicting the mean is
                        // dimmed: the number exists but says nothing useful.
                        ImGui::TextColored(v4(o.cv_session.r2 > 0.0 ? theme::kText : theme::kFaint),
                                           "%.3f", o.cv_session.r2);
                        ImGui::TableNextColumn();
                        if (o.has_subject_cv) {
                            // The gap between this and the session figure is the
                            // real measure of whether any of it transfers to a
                            // new person.
                            ImGui::TextColored(
                                v4(o.cv_subject.r2 > 0.0 ? theme::kText : theme::kFaint),
                                "%.3f", o.cv_subject.r2);
                        } else {
                            ImGui::TextColored(v4(theme::kFaint), "one subject");
                        }
                        ImGui::TableNextColumn();
                        ImGui::Text("%.4f", o.cv_session.rmse);

                        ImGui::TableNextColumn();
                        const Verdict v = verdict_for(st, m, k);
                        ImGui::TextColored(v4(verdict_color(v)), "%s",
                                           elanora::data::verdict_name(v));
                    }
                    ImGui::EndTable();
                }
            }
        }
        end_card();   // unconditional, like ImGui::EndChild
        ImGui::PopID();
        ImGui::Dummy(ImVec2(1, theme::kS2));
    }

    ImGui::EndChild();
}

void draw_forward(AppState& st) {
    if (!st.reg.any_trained()) {
        ImGui::TextColored(v4(theme::kMuted), "Train the models first.");
        return;
    }

    const auto [lo, hi] = st.reg.trained_freq_range();
    const float log_lo = static_cast<float>(std::log2(lo));
    const float log_hi = static_cast<float>(std::log2(hi));
    st.forward_log2f = std::clamp(st.forward_log2f, log_lo, log_hi);

    const double hz = std::exp2(static_cast<double>(st.forward_log2f));

    // Log-scaled, matching how the design was spaced and how the models consume
    // frequency. A linear slider would spend most of its travel above 20 Hz.
    ImGui::SetNextItemWidth(-260.0f);
    ImGui::SliderFloat("##freq", &st.forward_log2f, log_lo, log_hi, "");
    ImGui::SameLine();
    char hz_text[32];
    std::snprintf(hz_text, sizeof(hz_text), "%.2f", hz);
    readout(hz_text, "Hz", theme::kAccent);

    ImGui::TextColored(v4(theme::kFaint),
                       "baseline taken from the training mean; a state far outside it is "
                       "refused rather than answered");

    const Prediction p = predict(st.reg, hz, st.baseline);
    if (!p.baseline_in_range()) {
        ImGui::TextColored(v4(theme::kBad),
                           "baseline is %.1f SD outside the training range", p.max_baseline_z);
        return;
    }

    ImGui::Dummy(ImVec2(1, theme::kS3));

    // Predicted change as diverging bars rather than a table of signed numbers.
    // Direction becomes position and magnitude becomes length, so the shape of
    // the response reads before any single value does -- which is the whole
    // question this tab answers.
    //
    // The scale is shared across all 20 cells and set by the largest predicted
    // magnitude, so bar lengths are comparable between sensors. Scaling each
    // row to its own maximum would make a noise-level wobble look identical to
    // a real response.
    double full_scale = 0.0;
    for (int sn = 0; sn < kSensorCount; ++sn) {
        for (int b = 0; b < kBandCount; ++b) {
            const auto si = static_cast<std::size_t>(sn);
            const auto bi = static_cast<std::size_t>(b);
            full_scale = std::max(full_scale,
                                  std::abs(p.d_bands[si][bi]) + p.d_bands_sd[si][bi]);
        }
    }
    if (full_scale < 1e-9) full_scale = 1.0;

    // Laid out as a table rather than with SameLine arithmetic. Manual spacing
    // drifts by a pixel or two per cell, which is enough that the zero rules
    // stop lining up into a column and the bars read as scattered rather than
    // as a grid.
    const float cell_h = 24.0f;
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;

    if (ImGui::BeginTable("resp", kBandCount + 1, tf)) {
        ImGui::TableSetupColumn("sensor", ImGuiTableColumnFlags_WidthFixed, 84.0f);
        for (int b = 0; b < kBandCount; ++b) {
            ImGui::TableSetupColumn(band_name(static_cast<Band>(b)),
                                    ImGuiTableColumnFlags_WidthFixed, 128.0f);
        }

        // Header row, each band in its identity colour. The five hues name a
        // band and never encode a value; magnitude is carried by length alone.
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        for (int b = 0; b < kBandCount; ++b) {
            ImGui::TableNextColumn();
            const char* nm = band_name(static_cast<Band>(b));
            const float pad = (ImGui::GetContentRegionAvail().x -
                               ImGui::CalcTextSize(nm).x) * 0.5f;
            if (pad > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
            ImGui::TextColored(v4(theme::band_color(b)), "%s", nm);
        }

        for (int sn = 0; sn < kSensorCount; ++sn) {
            const SensorId sid = static_cast<SensorId>(sn);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, cell_h + 8.0f);

            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(v4(theme::kDim), "%s %s", sensor_name(sid),
                               electrode_name(sid));

            for (int b = 0; b < kBandCount; ++b) {
                ImGui::TableNextColumn();
                const auto si = static_cast<std::size_t>(sn);
                const auto bi = static_cast<std::size_t>(b);
                const double mu = p.d_bands[si][bi];
                const double sd = p.d_bands_sd[si][bi];

                const float w = ImGui::GetContentRegionAvail().x;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                diverging_bar(at, ImVec2(w, cell_h), mu, full_scale, sd,
                              theme::band_color(b));

                // The exact number stays on hover. Printing it beside every bar
                // would rebuild the wall of digits the bars replaced.
                ImGui::PushID(sn * kBandCount + b);
                if (ImGui::InvisibleButton("##cell", ImVec2(w, cell_h))) {
                    st.sel_model  = static_cast<ModelId>(sn);
                    st.sel_output = b;
                }
                // A hairline around the selected cell, drawn after the bar so
                // it is not covered by it.
                if (st.sel_model == static_cast<ModelId>(sn) && st.sel_output == b) {
                    ImGui::GetWindowDrawList()->AddRect(
                        ImVec2(at.x - 2.0f, at.y - 2.0f),
                        ImVec2(at.x + w + 2.0f, at.y + cell_h + 2.0f),
                        u32(theme::kAccent), 3.0f, 0, 1.5f);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s %s\npredicted %+.4f, sd %.4f\n%s",
                                      sensor_name(sid), band_name(static_cast<Band>(b)),
                                      mu, sd,
                                      std::abs(mu) > sd ? "direction resolved"
                                                        : "interval crosses zero");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(1, theme::kS2));
    ImGui::TextColored(v4(theme::kFaint),
                       "bar length is the predicted change, whisker is one standard "
                       "deviation; a dimmed bar means the interval crosses zero");

    // ---- heart and breathing ---------------------------------------------
    ImGui::Dummy(ImVec2(1, theme::kS3));
    {
        struct Row { const char* label; double mu, sd; const char* unit; };
        const Row rows[] = {
            {"heart",     p.d_bpm,       p.d_bpm_sd,       "BPM"},
            {"breathing", p.d_breathing, p.d_breathing_sd, "breaths/min"},
        };

        const ImGuiTableFlags hf = ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("scalars", 3, hf)) {
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, 300.0f);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, 240.0f);

            for (const Row& r : rows) {
                // Each scaled to its own units: a BPM change and a breaths/min
                // change are not comparable lengths, and pretending otherwise
                // would be worse than two separate scales.
                const double scale = std::max(std::abs(r.mu) + r.sd, 1.0);

                ImGui::TableNextRow(ImGuiTableRowFlags_None, cell_h + 8.0f);
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(v4(theme::kDim), "%s", r.label);

                ImGui::TableNextColumn();
                const float w = ImGui::GetContentRegionAvail().x;
                diverging_bar(ImGui::GetCursorScreenPos(), ImVec2(w, cell_h), r.mu, scale,
                              r.sd, theme::kAccent);
                ImGui::Dummy(ImVec2(w, cell_h));

                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                // "sd" spelled out rather than a +- glyph: the font atlas is
                // ASCII, so a real plus-minus would render as a question mark.
                ImGui::TextColored(v4(std::abs(r.mu) > r.sd ? theme::kText : theme::kFaint),
                                   "%+.2f %s   sd %.2f", r.mu, r.unit, r.sd);
            }
            ImGui::EndTable();
        }
    }

    // ---- the selected outcome across the whole trained range --------------
    ImGui::Dummy(ImVec2(1, theme::kS4));
    {
        const std::string name = (st.sel_model == ModelId::Heart)     ? "heart d_bpm"
                               : (st.sel_model == ModelId::Breathing) ? "breathing d_rate"
                               : std::string(sensor_name(static_cast<SensorId>(
                                     static_cast<int>(st.sel_model)))) + " " +
                                     band_name(static_cast<Band>(st.sel_output));
        eyebrow("RESPONSE ACROSS FREQUENCY");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme::kS2);
        ImGui::TextColored(v4(theme::kMuted), "%s", name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(v4(theme::kFaint), "- click a bar above to change");

        ImGui::Dummy(ImVec2(1, theme::kS1));

        // Sampled on the same log2 grid the optimizer sweeps, so the curve the
        // operator reads and the curve the optimizer scores are the same shape.
        std::vector<double> fx, fy;
        fx.reserve(120);
        fy.reserve(120);
        for (int i = 0; i < 120; ++i) {
            const double t = static_cast<double>(i) / 119.0;
            const double l2 = log_lo + t * (log_hi - log_lo);
            const double f = std::exp2(l2);
            const Prediction q = predict(st.reg, f, st.baseline);

            double v = 0.0;
            if (st.sel_model == ModelId::Heart) {
                v = q.d_bpm;
            } else if (st.sel_model == ModelId::Breathing) {
                v = q.d_breathing;
            } else {
                v = q.d_bands[static_cast<std::size_t>(st.sel_model)]
                             [static_cast<std::size_t>(st.sel_output)];
            }
            fx.push_back(l2);
            fy.push_back(v);
        }

        const float cw = std::min(920.0f, ImGui::GetContentRegionAvail().x - theme::kS4);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        // The whole x range IS the trained range here, so nothing is shaded as
        // extrapolation -- the sweep is clamped to it by construction.
        score_curve(at, ImVec2(cw, 150.0f), fx, fy, 0.0, 0.0,
                    static_cast<double>(st.forward_log2f), theme::kAccent);
        ImGui::Dummy(ImVec2(cw, 150.0f));

        // Axis labels at the octave marks the design was actually spaced on.
        ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kFaint));
        for (double f : st.reg.trained_frequencies()) {
            const float x = at.x + static_cast<float>((std::log2(f) - log_lo) /
                                                      (log_hi - log_lo)) * cw;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(x, at.y + 150.0f),
                                                ImVec2(x, at.y + 155.0f),
                                                u32(theme::kLineHi), 1.0f);
        }
        ImGui::Text("%.2f Hz", lo);
        ImGui::SameLine();
        right_align(70.0f);
        ImGui::Text("%.1f Hz", hi);
        ImGui::PopStyleColor();
        ImGui::TextColored(v4(theme::kFaint),
                           "ticks mark the frequencies actually presented; between them the "
                           "model is interpolating");
    }

    ImGui::Dummy(ImVec2(1, theme::kS3));
    ImGui::TextColored(v4(theme::kFaint),
                       "%.2f Hz is associated with these predicted changes. This is an "
                       "association, not a demonstrated cause.", hz);
}

void draw_inverse(AppState& st) {
    if (!st.reg.any_trained()) {
        ImGui::TextColored(v4(theme::kMuted), "Train the models first.");
        return;
    }

    static const char* kGoals[] = {"ignore", "increase", "decrease", "target"};

    // The tab scrolls as a whole: goals, result and the score sweep together
    // run past the bottom of a 900 px display, and a curve nobody can reach is
    // the same as no curve.
    ImGui::BeginChild("invscroll", ImVec2(0, 0), ImGuiChildFlags_None);

    ImGui::Columns(2, "invcols", false);
    ImGui::SetColumnWidth(0, 600.0f);

    eyebrow("DESIRED STATE");
    ImGui::Dummy(ImVec2(1, theme::kS1));

    // Fixed widths, not stretch: proportional sizing squeezed the first band
    // column until "ignore" truncated to "ig", which is not a word.
    const ImGuiTableFlags f = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("goals", 6, f)) {
        ImGui::TableSetupColumn("sensor", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        for (int b = 0; b < kBandCount; ++b) {
            ImGui::TableSetupColumn(band_name(static_cast<Band>(b)),
                                    ImGuiTableColumnFlags_WidthFixed, 94.0f);
        }
        ImGui::TableHeadersRow();

        for (int s = 0; s < kSensorCount; ++s) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(sensor_name(static_cast<SensorId>(s)));
            for (int b = 0; b < kBandCount; ++b) {
                ImGui::TableNextColumn();
                ImGui::PushID(s * kBandCount + b);
                const auto si = static_cast<std::size_t>(s);
                const auto bi = static_cast<std::size_t>(b);
                int g = static_cast<int>(st.desired.brain[si][bi].goal);
                ImGui::SetNextItemWidth(86.0f);
                if (ImGui::Combo("##g", &g, kGoals, 4)) {
                    st.desired.brain[si][bi].goal = static_cast<Goal>(g);
                }
                // A goal on an unsupported outcome is flagged here, before the
                // user runs anything and gets a refusal they did not expect.
                if (g != 0 && verdict_for(st, static_cast<ModelId>(s), b) != Verdict::Supported) {
                    ImGui::TextColored(v4(theme::kWarn), "no evidence");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Dummy(ImVec2(1, theme::kS2));
    {
        int gh = static_cast<int>(st.desired.heart.goal);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("heart", &gh, kGoals, 4)) st.desired.heart.goal = static_cast<Goal>(gh);
        int gb = static_cast<int>(st.desired.breathing.goal);
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::Combo("breathing", &gb, kGoals, 4)) {
            st.desired.breathing.goal = static_cast<Goal>(gb);
        }
    }

    ImGui::Dummy(ImVec2(1, theme::kS2));
    // Through float temporaries. Pointing SliderFloat at a double would have it
    // read and write the first four bytes of an eight-byte value -- it compiles
    // cleanly and produces garbage.
    float unc = static_cast<float>(st.desired.uncertainty_penalty);
    float ext = static_cast<float>(st.desired.extrapolation_penalty);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("uncertainty penalty", &unc, 0.0f, 2.0f, "%.2f")) {
        st.desired.uncertainty_penalty = unc;
    }
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("extrapolation penalty", &ext, 0.0f, 3.0f, "%.2f")) {
        st.desired.extrapolation_penalty = ext;
    }
    ImGui::Checkbox("require effect floor", &st.desired.require_effect_floor);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Refuse any recommendation whose predicted change is smaller\n"
                          "than twice the variation seen on control trials.");
    }

    ImGui::Dummy(ImVec2(1, theme::kS2));
    if (ImGui::Button("Find frequency", ImVec2(180.0f, 34.0f))) {
        st.result = optimize(st.reg, st.evidence, st.desired, st.baseline);
        st.have_result = true;
    }

    // ---- results ----------------------------------------------------------
    ImGui::NextColumn();
    eyebrow("RESULT");
    ImGui::Dummy(ImVec2(1, theme::kS1));

    if (!st.have_result) {
        ImGui::TextColored(v4(theme::kMuted), "Set a goal and press Find frequency.");
    } else if (!st.result.found) {
        // The refusal is the headline, not a footnote under a number.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(theme::kRaised));
        ImGui::BeginChild("refuse", ImVec2(0, 150.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
        status_pill("no recommendation", theme::kWarn);
        ImGui::Dummy(ImVec2(1, theme::kS1));
        ImGui::TextWrapped("%s", st.result.reason.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        const Candidate& best = st.result.ranked.front();
        char hz[32];
        std::snprintf(hz, sizeof(hz), "%.2f", best.frequency_hz);
        readout(hz, "Hz", theme::kAccent);
        ImGui::TextColored(v4(theme::kFaint),
                           "associated with a predicted response matching the goal "
                           "(score %.2f)", best.score);

        ImGui::Dummy(ImVec2(1, theme::kS2));
        ImGui::Text("goal match         %+.3f", best.goal_match);
        ImGui::Text("uncertainty cost   -%.3f", best.uncertainty_cost);
        ImGui::Text("extrapolation cost -%.3f", best.extrapolation_cost);

        ImGui::Dummy(ImVec2(1, theme::kS2));
        eyebrow("RANKED");
        const ImGuiTableFlags rf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                   ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("ranked", 3, rf, ImVec2(0, 220.0f))) {
            ImGui::TableSetupColumn("Hz");
            ImGui::TableSetupColumn("score");
            ImGui::TableSetupColumn("sd");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < st.result.ranked.size() && i < 20; ++i) {
                const Candidate& c = st.result.ranked[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", c.frequency_hz);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", c.score);
                ImGui::TableNextColumn();
                ImGui::TextColored(v4(theme::kMuted), "%.3f", c.uncertainty_cost);
            }
            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(1, theme::kS2));
        if (ImGui::Button("Export for collector", ImVec2(200.0f, 30.0f))) {
            // Closes the loop: the collector's setup screen can read this and
            // run a denser sweep around the region the models pointed at.
            std::error_code ec;
            fs::create_directories(st.trained, ec);
            CsvWriter w(st.trained / "handoff.csv", {"frequency_hz", "score", "response"});
            for (std::size_t i = 0; i < st.result.ranked.size() && i < 8; ++i) {
                w.row(std::vector<std::string>{fmt6(st.result.ranked[i].frequency_hz),
                                               fmt6(st.result.ranked[i].score),
                                               st.reg.options().response});
            }
            st.status = "wrote handoff.csv";
        }
    }
    ImGui::Columns(1);

    // ---- the score sweep, full width -------------------------------------
    //
    // The ranked table says which frequency won; the curve says whether it won
    // by a nose or by a mile. A flat curve with a nominal winner is exactly the
    // case the min_score threshold exists to refuse, and seeing its shape is
    // the difference between trusting a recommendation and understanding it.
    if (st.have_result && !st.result.ranked.empty()) {
        ImGui::Dummy(ImVec2(1, theme::kS3));
        eyebrow("SCORE ACROSS FREQUENCY");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + theme::kS2);
        ImGui::TextColored(v4(theme::kFaint),
                           st.result.found ? "winner marked; shaded band is the trained range"
                                           : "no winner: the curve is too flat to choose from");
        ImGui::Dummy(ImVec2(1, theme::kS1));

        // ranked is sorted by score; the curve needs it ordered by frequency.
        std::vector<std::pair<double, double>> by_freq;
        by_freq.reserve(st.result.ranked.size());
        for (const Candidate& c : st.result.ranked) {
            by_freq.emplace_back(std::log2(c.frequency_hz), c.score);
        }
        std::sort(by_freq.begin(), by_freq.end());

        std::vector<double> cx, cy;
        cx.reserve(by_freq.size());
        cy.reserve(by_freq.size());
        for (const auto& [x, y] : by_freq) { cx.push_back(x); cy.push_back(y); }

        const auto [lo, hi] = st.reg.trained_freq_range();
        const double mark = st.result.found
                                ? std::log2(st.result.ranked.front().frequency_hz)
                                : -1e9;

        const float cw = ImGui::GetContentRegionAvail().x - theme::kS4;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        score_curve(at, ImVec2(cw, 170.0f), cx, cy, std::log2(lo), std::log2(hi), mark,
                    st.result.found ? theme::kAccent : theme::kWarn);
        ImGui::Dummy(ImVec2(cw, 170.0f));

        ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kFaint));
        ImGui::Text("%.2f Hz", lo);
        ImGui::SameLine();
        right_align(70.0f);
        ImGui::Text("%.1f Hz", hi);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1, theme::kS4));
    }

    ImGui::EndChild();
}

}  // namespace

int main(int argc, char** argv) {
    AppState st;
    // Automation flags, so the app can be smoke-tested and captured without a
    // person driving it.
    int max_frames = -1;
    std::string shot;
    int shot_tab = -1;

    // Training is a batch job; requiring a window for it would make this
    // unusable from a script or over a remote shell.
    bool headless_train = false;
    // "S1:alpha:increase" and the like, so a recommendation can be requested
    // from a script and the refusal paths exercised without a window.
    std::string recommend;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) { max_frames = std::atoi(argv[++i]); continue; }
        if (a == "--screenshot" && i + 1 < argc) { shot = argv[++i]; continue; }
        if (a == "--tab" && i + 1 < argc) { shot_tab = std::atoi(argv[++i]); continue; }
        if (a == "--datasets" && i + 1 < argc) { st.datasets = argv[++i]; continue; }
        if (a == "--trained" && i + 1 < argc) { st.trained = argv[++i]; continue; }
        if (a == "--train") { headless_train = true; continue; }
        if (a == "--recommend" && i + 1 < argc) { recommend = argv[++i]; continue; }
    }

    if (headless_train) {
        st.reg.train_all(st.datasets);
        for (int i = 0; i < kModelCount; ++i) {
            const ModelId m = static_cast<ModelId>(i);
            if (st.reg.trained(m)) {
                std::printf("%-11s trained, n=%d\n", model_id_name(m), st.reg.n_samples(m));
            } else {
                std::printf("%-11s NOT trained: %s\n", model_id_name(m),
                            st.reg.untrained_reason(m).c_str());
            }
        }
        std::string err;
        if (!st.reg.save(st.trained, err)) {
            std::fprintf(stderr, "save failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("saved to %s\n", st.trained.string().c_str());
        return 0;
    }

    if (!recommend.empty()) {
        std::string err;
        if (!st.reg.load(st.trained, err)) {
            std::fprintf(stderr, "no trained models: %s\n", err.c_str());
            return 1;
        }
        st.evidence = elanora::data::read_evidence(st.datasets);
        st.baseline = BaselineState::from_registry(st.reg);

        // sensor:band:direction
        int sensor = 0, band = 0;
        Goal g = Goal::Increase;
        {
            const std::size_t a1 = recommend.find(':');
            const std::size_t a2 = recommend.find(':', a1 + 1);
            const std::string s_txt = recommend.substr(0, a1);
            const std::string b_txt = recommend.substr(a1 + 1, a2 - a1 - 1);
            const std::string d_txt = (a2 == std::string::npos) ? "increase"
                                                                : recommend.substr(a2 + 1);
            for (int i2 = 0; i2 < kSensorCount; ++i2) {
                if (s_txt == sensor_name(static_cast<SensorId>(i2))) sensor = i2;
            }
            for (int i2 = 0; i2 < kBandCount; ++i2) {
                if (b_txt == band_name(static_cast<Band>(i2))) band = i2;
            }
            if (d_txt == "decrease") g = Goal::Decrease;
        }
        st.desired.brain[static_cast<std::size_t>(sensor)][static_cast<std::size_t>(band)] =
            {g, 0.0, 1.0};

        st.result = optimize(st.reg, st.evidence, st.desired, st.baseline);
        st.have_result = true;

        // Without --frames this is a pure command-line query and no window is
        // ever opened. With it, the goal is simply left preset so the Inverse
        // tab comes up already populated.
        if (max_frames < 0) {
            const OptimizerResult& r = st.result;
            if (r.found) {
                std::printf("%.2f Hz is associated with a predicted %s in %s %s (score %.2f)\n",
                            r.ranked.front().frequency_hz, goal_name(g),
                            sensor_name(static_cast<SensorId>(sensor)),
                            band_name(static_cast<Band>(band)), r.ranked.front().score);
                std::printf("   This is an association, not a demonstrated cause.\n");
            } else {
                std::printf("No recommendation: %s\n", r.reason.c_str());
            }
            return 0;
        }
    }

    UiShell shell("ELANORA - Models", 1560, 960);
    if (!shell.ok()) {
        std::fprintf(stderr, "could not open a window\n");
        return 1;
    }

    st.evidence = elanora::data::read_evidence(st.datasets);
    {
        std::string err;
        if (st.reg.load(st.trained, err)) {
            st.baseline = BaselineState::from_registry(st.reg);
            st.status = "loaded trained models";
        }
    }

    int frames = 0;
    while (shell.begin_frame()) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        draw_evidence_banner(st);
        ImGui::Dummy(ImVec2(1, theme::kS2));

        // --tab selects a tab for the first few frames so a capture can target
        // it; after that the selection is the user's again.
        auto tab_flags = [&](int want) {
            return (shot_tab == want && frames < 3) ? ImGuiTabItemFlags_SetSelected
                                                    : ImGuiTabItemFlags_None;
        };

        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Train", nullptr, tab_flags(0)))   { draw_train(st);   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Forward", nullptr, tab_flags(1))) { draw_forward(st); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Inverse", nullptr, tab_flags(2))) { draw_inverse(st); ImGui::EndTabItem(); }
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
