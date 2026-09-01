#include "elanora/collector/view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "elanora/render.hpp"
#include "elanora/theme.hpp"

#include "imgui.h"

namespace elanora::collector {

namespace {

ImVec4 v4(theme::Rgba c) { return ImVec4{c.r, c.g, c.b, c.a}; }

theme::Rgba condition_color(Condition c) {
    switch (c) {
        case Condition::Stim:          return theme::kAlpha;
        case Condition::ControlJitter: return theme::kDelta;
        case Condition::ControlTone:   return theme::kWarn;
    }
    return theme::kMuted;
}

const char* condition_label(Condition c) {
    switch (c) {
        case Condition::Stim:          return "isochronic";
        case Condition::ControlJitter: return "jitter control";
        case Condition::ControlTone:   return "tone control";
    }
    return "";
}

const char* phase_label(Phase p) {
    switch (p) {
        case Phase::Baseline: return "Baseline";
        case Phase::Stimulus: return "Stimulus";
        case Phase::Post:     return "Post";
        case Phase::Rest:     return "Rest";
    }
    return "";
}

// One lane per enabled layer, each pulse drawn as a rounded block.
//
// The previous version filled a rectangle with one vertical line per pixel
// column, which produced a hard-edged barcode with no shape and no way to tell
// the layers apart. Pulses are geometry, so they are drawn as geometry: one
// rounded rect per gate opening, in that layer's colour, on its own row.
//
// Separate rows rather than a summed blob because the useful question about a
// stacked design is which rhythms are running and where they coincide, and a
// single summed trace answers neither.
void draw_stimulus_lanes(const StimulusDesign& d, ImVec2 origin, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr double kWindow = 1.0;   // seconds shown

    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      ImGui::GetColorU32(v4(theme::kGround)), theme::kRadiusSm);

    const float label_w = 64.0f;
    const float axis_h  = 15.0f;
    const float plot_x  = origin.x + label_w;
    const float plot_w  = size.x - label_w - theme::kS3;

    // Quarter-second guides, behind the pulses.
    for (int i = 1; i < 4; ++i) {
        const float x = plot_x + plot_w * (i / 4.0f);
        dl->AddLine(ImVec2(x, origin.y + 6.0f), ImVec2(x, origin.y + size.y - axis_h),
                    ImGui::GetColorU32(ImVec4(theme::kLine.r, theme::kLine.g,
                                              theme::kLine.b, 0.7f)), 1.0f);
    }

    // Only enabled layers get a row. In Single mode just the one that plays,
    // because showing rows for frequencies that will not sound is a lie about
    // what the round does.
    std::vector<int> rows;
    for (int i = 0; i < static_cast<int>(d.layers.size()); ++i) {
        if (!d.layers[static_cast<std::size_t>(i)].enabled) continue;
        rows.push_back(i);
        if (d.mode == StimMode::Single) break;
    }

    if (rows.empty()) {
        const char* msg = "no layers enabled";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(origin.x + (size.x - ts.x) * 0.5f,
                           origin.y + (size.y - ts.y) * 0.5f),
                    ImGui::GetColorU32(v4(theme::kFaint)), msg);
        return;
    }

    const float body_h = size.y - axis_h - theme::kS2;
    const float gap    = 6.0f;
    const float lane_h = std::min(30.0f,
        (body_h - gap * (rows.size() - 1)) / static_cast<float>(rows.size()));

    for (std::size_t r = 0; r < rows.size(); ++r) {
        const Layer& l = d.layers[static_cast<std::size_t>(rows[r])];
        const theme::Rgba col = theme::band_color(rows[r] % kBandCount);
        const float top = origin.y + theme::kS2 + (lane_h + gap) * static_cast<float>(r);

        // Recessed track for the lane, so a silent gap still reads as part of
        // the row rather than as empty card.
        dl->AddRectFilled(ImVec2(plot_x, top), ImVec2(plot_x + plot_w, top + lane_h),
                          ImGui::GetColorU32(ImVec4(1, 1, 1, 0.03f)), lane_h * 0.35f);

        if (fonts().mono) ImGui::PushFont(fonts().mono);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.1f Hz", l.hz);
        dl->AddText(ImVec2(origin.x + theme::kS2,
                           top + (lane_h - ImGui::GetTextLineHeight()) * 0.5f),
                    ImGui::GetColorU32(ImVec4(col.r, col.g, col.b, 0.9f)), lbl);
        if (fonts().mono) ImGui::PopFont();

        // Pulse geometry, computed rather than sampled: period from the rate,
        // width from the duty cycle. Amplitude sets the block height so a quiet
        // layer looks quiet.
        const double period = 1.0 / std::max(l.hz, 0.01);
        const float  amp_h  = lane_h * static_cast<float>(std::clamp(l.amp, 0.1, 1.0));
        const float  y0     = top + (lane_h - amp_h) * 0.5f;
        const float  r_px   = std::min(4.0f, amp_h * 0.4f);

        for (int k = 0; k < 4000; ++k) {
            const double t0 = period * k;
            if (t0 >= kWindow) break;
            const double t1 = std::min(t0 + period * d.duty, kWindow);

            const float x0 = plot_x + static_cast<float>(t0 / kWindow) * plot_w;
            const float x1 = plot_x + static_cast<float>(t1 / kWindow) * plot_w;
            // Never thinner than a couple of pixels: at 45 Hz a true-width
            // pulse would vanish and the lane would look empty.
            const float w = std::max(2.5f, x1 - x0);

            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + amp_h),
                              ImGui::GetColorU32(ImVec4(col.r, col.g, col.b, 0.92f)),
                              r_px);
        }
    }

    // Time axis.
    if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
    for (int i = 0; i <= 4; ++i) {
        char t[12];
        std::snprintf(t, sizeof(t), "%.2fs", i * 0.25);
        const float x = plot_x + plot_w * (i / 4.0f);
        const ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(x - (i == 0 ? 0.0f : i == 4 ? ts.x : ts.x * 0.5f),
                           origin.y + size.y - axis_h + 2.0f),
                    ImGui::GetColorU32(v4(theme::kFaint)), t);
    }
    if (fonts().eyebrow) ImGui::PopFont();
}

// The 20 rounds as pills: done, current, upcoming, with the condition carried
// by an underline so controls are visible in the plan at a glance.
void draw_schedule(const std::vector<PlannedRound>& sched, int current,
                   ImVec2 origin, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sched.empty()) return;

    const int n = static_cast<int>(sched.size());
    const float gap = 5.0f;
    const float w = (width - gap * (n - 1)) / n;
    const float h = 26.0f;

    for (int i = 0; i < n; ++i) {
        const ImVec2 p(origin.x + (w + gap) * i, origin.y);
        const bool done = i < current;
        const bool now  = i == current;

        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                          ImGui::GetColorU32(v4(now ? theme::kRaised : theme::kGround)),
                          theme::kRadiusSm);
        if (now) {
            dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                        ImGui::GetColorU32(v4(theme::kAccent)), theme::kRadiusSm, 0, 1.5f);
        }

        const theme::Rgba cc = condition_color(sched[static_cast<std::size_t>(i)].cond);
        dl->AddRectFilled(ImVec2(p.x + 4.0f, p.y + h - 4.0f),
                          ImVec2(p.x + w - 4.0f, p.y + h - 2.0f),
                          ImGui::GetColorU32(ImVec4(cc.r, cc.g, cc.b, done ? 1.0f : 0.30f)),
                          1.0f);

        char lbl[8];
        std::snprintf(lbl, sizeof(lbl), "%d", i + 1);
        if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + 5.0f),
                    ImGui::GetColorU32(v4(now ? theme::kText
                                        : done ? theme::kDim : theme::kFaint)), lbl);
        if (fonts().eyebrow) ImGui::PopFont();
    }
}

// Baseline / Stimulus / Post / Rest as proportional segments, the active one
// filling. The countdown says how long is left in this phase; this says where
// the phase sits in the round.
void draw_phase_strip(const TrialRunner& t, const Durations& d,
                      ImVec2 origin, float width, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static const Phase kPhases[] = {Phase::Baseline, Phase::Stimulus, Phase::Post, Phase::Rest};

    const float total = static_cast<float>(d.round_total());
    float x = origin.x;
    for (int i = 0; i < 4; ++i) {
        const Phase p = kPhases[i];
        const float w = width * static_cast<float>(d.of(p)) / total - 3.0f;
        const bool done = static_cast<int>(p) < static_cast<int>(t.phase());
        const bool now  = p == t.phase();

        dl->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + w, origin.y + height),
                          ImGui::GetColorU32(v4(theme::kGround)), theme::kRadiusSm);
        if (done || now) {
            const float f = done ? 1.0f
                                 : static_cast<float>(t.phase_elapsed() / d.of(p));
            const theme::Rgba c = (p == Phase::Stimulus) ? theme::kAlpha : theme::kAccent;
            dl->AddRectFilled(ImVec2(x, origin.y),
                              ImVec2(x + w * std::clamp(f, 0.0f, 1.0f), origin.y + height),
                              ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, done ? 0.35f : 0.9f)),
                              theme::kRadiusSm);
        }

        if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
        const char* nm = phase_label(p);
        const ImVec2 ts = ImGui::CalcTextSize(nm);
        dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, origin.y + (height - ts.y) * 0.5f),
                    ImGui::GetColorU32(v4(now ? theme::kGround : theme::kMuted)), nm);
        if (fonts().eyebrow) ImGui::PopFont();

        x += w + 3.0f;
    }
}

void draw_survey_modal(CollectorState& st) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS5, theme::kS5));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::kRadius);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, v4(theme::kPanel));

    ImGui::Begin("##survey", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_AlwaysAutoResize);

    char title[64];
    std::snprintf(title, sizeof(title), "Round %d of %d complete",
                  st.runner.round_index() + 1, st.runner.round_count());
    if (fonts().subhead) ImGui::PushFont(fonts().subhead);
    ImGui::TextUnformatted(title);
    if (fonts().subhead) ImGui::PopFont();

    const PlannedRound& r = st.runner.current();
    if (r.cond == Condition::Stim) {
        ImGui::TextColored(v4(theme::kMuted), "%.1f Hz isochronic - 90 s recorded", r.hz);
    } else {
        ImGui::TextColored(v4(theme::kMuted), "%s - 90 s recorded", condition_label(r.cond));
    }
    ImGui::Dummy(ImVec2(1, theme::kS3));

    auto scale = [](const char* id, const char* q, const char* lo, const char* hi, int* value) {
        ImGui::TextColored(v4(theme::kDim), "%s", q);
        ImGui::Dummy(ImVec2(1, 4));
        const float w = (ImGui::GetContentRegionAvail().x - 6.0f * 6.0f) / 7.0f;
        for (int i = 1; i <= 7; ++i) {
            if (i > 1) ImGui::SameLine(0.0f, 6.0f);
            const bool on = (*value == i);
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, v4(theme::kAccent));
                ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kGround));
            }
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%d##%s%d", i, id, i);
            if (ImGui::Button(lbl, ImVec2(w, 34.0f))) *value = i;
            if (on) ImGui::PopStyleColor(2);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kFaint));
        ImGui::TextUnformatted(lo);
        ImGui::SameLine();
        right_align(ImGui::CalcTextSize(hi).x);
        ImGui::TextUnformatted(hi);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(1, theme::kS3));
    };

    scale("rx", "How relaxed do you feel right now?", "Not at all", "Completely",
          &st.survey.relaxation);
    scale("al", "How alert do you feel right now?", "Very drowsy", "Wide awake",
          &st.survey.alertness);

    // The manipulation check. If subjects report a rhythm on jitter controls as
    // often as on stimulus rounds, the control is not working and the jitter
    // needs widening -- without this question there is no way to know.
    ImGui::TextColored(v4(theme::kDim), "Did you notice a steady rhythm in the sound?");
    ImGui::Dummy(ImVec2(1, 4));
    static const char* kRhythm[] = {"Yes, clear", "Faint or unsure", "No rhythm"};
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 6.0f);
        const bool on = (st.survey.rhythm == i);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, v4(theme::kAccent));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kGround));
        }
        if (ImGui::Button(kRhythm[i], ImVec2(0, 32.0f))) st.survey.rhythm = i;
        if (on) ImGui::PopStyleColor(2);
    }
    ImGui::Dummy(ImVec2(1, theme::kS3));

    ImGui::TextColored(v4(theme::kDim), "Anything that could have disturbed the recording?");
    ImGui::TextColored(v4(theme::kFaint), "Optional");
    ImGui::Dummy(ImVec2(1, 4));
    struct Chip { const char* label; bool* flag; };
    const Chip chips[] = {
        {"Jaw clench",   &st.survey.jaw},
        {"Moved head",   &st.survey.moved},
        {"Eyes open",    &st.survey.eyes_open},
        {"Swallowed",    &st.survey.swallowed},
        {"Outside noise",&st.survey.noise},
    };
    for (int i = 0; i < 5; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 6.0f);
        const bool on = *chips[i].flag;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, v4(theme::kWarn));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kGround));
        }
        if (ImGui::Button(chips[i].label, ImVec2(0, 30.0f))) *chips[i].flag = !on;
        if (on) ImGui::PopStyleColor(2);
    }

    ImGui::Dummy(ImVec2(1, theme::kS4));
    const bool ok = st.survey.complete();
    ImGui::TextColored(v4(ok ? theme::kMuted : theme::kWarn), "%s",
                       ok ? "90 s recorded - ready to continue"
                          : "Answer the first three to continue");
    ImGui::SameLine();
    right_align(190.0f);
    ImGui::BeginDisabled(!ok);
    ImGui::PushStyleColor(ImGuiCol_Button, v4(theme::kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kGround));
    const bool last = st.runner.round_index() + 1 >= st.runner.round_count();
    if (ImGui::Button(last ? "Save & finish trial" : "Save & next round",
                      ImVec2(190.0f, 38.0f))) {
        st.runner.advance_round();
        st.survey.clear();
        st.survey_open = false;
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

}  // namespace

CollectorState::CollectorState() {
    design.layers = {{10.0, 1.0, true}, {6.0, 0.7, true}, {40.0, 0.45, false}};
}

std::vector<PlannedRound> CollectorState::preview_schedule() const {
    return build_schedule(geometric_set(freq_lo, freq_hi, freq_count),
                          n_jitter, n_tone, seed);
}

void draw_collector(CollectorState& st,
                    const std::array<lsl::ChannelQuality, kSensorCount>& qual,
                    float dt) {
    // The clock only advances here, never inside a draw helper, so a trial
    // cannot be advanced twice by a layout change.
    if (st.runner.running() && !st.survey_open) {
        if (st.runner.tick(dt * st.speed)) st.survey_open = true;
    }

    const float gap = theme::kS4;

    if (!st.runner.running() && !st.runner.finished()) {
        // ---------------------------------------------------------- setup
        const float avail = ImGui::GetContentRegionAvail().x;
        const float left_w = (avail - gap) * 0.62f;
        const float right_w = (avail - gap) * 0.38f;
        const float row_h = 430.0f;

        if (begin_card("##maker", ImVec2(left_w, row_h))) {
            eyebrow("Frequency maker");
            ImGui::SameLine();
            right_align(180.0f);
            static const char* kModes[] = {"Single", "Stacked"};
            int mode = (st.design.mode == StimMode::Stacked) ? 1 : 0;
            if (segmented("##mode", kModes, 2, &mode, 172.0f)) {
                st.design.mode = mode == 1 ? StimMode::Stacked : StimMode::Single;
            }

            ImGui::Dummy(ImVec2(1, theme::kS2));
            ImGui::TextColored(v4(theme::kMuted), "%s",
                st.design.mode == StimMode::Single
                    ? "Each round plays one frequency alone; the trial sweeps the list."
                    : "Every enabled layer sounds together, normalised so adding one "
                      "cannot raise the volume.");
            ImGui::Dummy(ImVec2(1, theme::kS3));

            int remove = -1;
            for (int i = 0; i < static_cast<int>(st.design.layers.size()); ++i) {
                Layer& l = st.design.layers[static_cast<std::size_t>(i)];
                ImGui::PushID(i);

                const ImVec2 dot = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##on", ImVec2(18.0f, 24.0f))) l.enabled = !l.enabled;
                const theme::Rgba lc = theme::band_color(i % kBandCount);
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(dot.x + 7.0f, dot.y + 12.0f), 6.0f,
                    ImGui::GetColorU32(ImVec4(lc.r, lc.g, lc.b, l.enabled ? 1.0f : 0.22f)), 16);

                ImGui::SameLine(0.0f, theme::kS2);
                ImGui::SetNextItemWidth(92.0f);
                float hz = static_cast<float>(l.hz);
                if (ImGui::DragFloat("##hz", &hz, 0.1f, 0.5f, 60.0f, "%.1f Hz")) {
                    l.hz = std::clamp(static_cast<double>(hz), 0.5, 60.0);
                }

                ImGui::SameLine(0.0f, theme::kS3);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 96.0f);
                // Slider works in whole percent. Formatting a 0..1 value with
                // a percent format printed "1%" for full amplitude.
                int amp_pct = static_cast<int>(std::lround(l.amp * 100.0));
                if (ImGui::SliderInt("##amp", &amp_pct, 10, 100, "%d%%",
                                     ImGuiSliderFlags_AlwaysClamp)) {
                    l.amp = amp_pct / 100.0;
                }

                ImGui::SameLine(0.0f, theme::kS3);
                if (ImGui::Button("Remove", ImVec2(72.0f, 0))) remove = i;
                ImGui::PopID();
            }
            if (remove >= 0 && st.design.layers.size() > 1) {
                st.design.layers.erase(st.design.layers.begin() + remove);
            }

            ImGui::Dummy(ImVec2(1, theme::kS1));
            if (st.design.layers.size() < 6 && ImGui::Button("Add layer", ImVec2(110.0f, 0))) {
                st.design.layers.push_back(Layer{8.0, 0.6, true});
            }
            ImGui::SameLine();
            right_align(230.0f);
            ImGui::TextColored(v4(theme::kFaint), "gate pattern, 1 second");

            const ImVec2 eo = ImGui::GetCursorScreenPos();
            const float ew = ImGui::GetContentRegionAvail().x;
            const float eh = std::max(40.0f, ImGui::GetContentRegionAvail().y - 4.0f);
            draw_stimulus_lanes(st.design, eo, ImVec2(ew, eh));
            ImGui::Dummy(ImVec2(ew, eh));
        }
        end_card();

        ImGui::SameLine(0.0f, gap);
        if (begin_card("##cfg", ImVec2(right_w, row_h))) {
            eyebrow("Trial");
            ImGui::Dummy(ImVec2(1, theme::kS3));

            auto row = [](const char* k, const char* fmt, ...) {
                ImGui::TextColored(v4(theme::kMuted), "%s", k);
                ImGui::SameLine();
                va_list args;
                va_start(args, fmt);
                char buf[64];
                std::vsnprintf(buf, sizeof(buf), fmt, args);
                va_end(args);
                right_align(ImGui::CalcTextSize(buf).x);
                if (fonts().mono) ImGui::PushFont(fonts().mono);
                ImGui::TextColored(v4(theme::kDim), "%s", buf);
                if (fonts().mono) ImGui::PopFont();
            };

            row("Subject", "%s", st.subject.c_str());
            row("Trial", "%s", st.trial_id.c_str());
            row("Baseline / Stim / Post", "%.0f / %.0f / %.0f s",
                st.durations.baseline, st.durations.stimulus, st.durations.post);
            row("Rest", "%.0f s", st.durations.rest);
            row("Carrier", "%.0f Hz", st.design.carrier_hz);
            row("Duty", "%.0f %%", st.design.duty * 100.0);
            row("Seed", "%llu", static_cast<unsigned long long>(st.seed));

            ImGui::Dummy(ImVec2(1, theme::kS3));
            const auto sched = st.preview_schedule();
            const int rounds = static_cast<int>(sched.size());
            const double mins = st.durations.round_total() * rounds / 60.0;

            if (fonts().metric) ImGui::PushFont(fonts().metric);
            ImGui::TextColored(v4(theme::kText), "%d", rounds);
            if (fonts().metric) ImGui::PopFont();
            ImGui::SameLine(0.0f, theme::kS2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14.0f);
            ImGui::TextColored(v4(theme::kMuted), "rounds  -  %.0f min", mins);

            ImGui::Dummy(ImVec2(1, theme::kS2));
            ImGui::TextColored(v4(theme::kFaint),
                               "%d stimulus, %d jitter, %d tone controls",
                               st.freq_count, st.n_jitter, st.n_tone);

            ImGui::Dummy(ImVec2(1, theme::kS3));
            ImGui::PushStyleColor(ImGuiCol_Button, v4(theme::kAccent));
            ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kGround));
            if (ImGui::Button("Start trial", ImVec2(-1.0f, 42.0f))) {
                st.runner.start(sched, st.durations);
                st.survey.clear();
                st.survey_open = false;
            }
            ImGui::PopStyleColor(2);
        }
        end_card();

        if (begin_card("##sched", ImVec2(0, 96.0f))) {
            eyebrow("Round schedule");
            ImGui::SameLine();
            right_align(300.0f);
            ImGui::TextColored(v4(theme::kFaint),
                               "randomised - controls never adjacent or at either end");
            ImGui::Dummy(ImVec2(1, theme::kS2));
            draw_schedule(st.preview_schedule(), -1, ImGui::GetCursorScreenPos(),
                          ImGui::GetContentRegionAvail().x);
        }
        end_card();
        return;
    }

    if (st.runner.finished()) {
        if (begin_card("##done", ImVec2(0, 0))) {
            const ImVec2 p0 = ImGui::GetWindowPos();
            const float cx = p0.x + ImGui::GetWindowWidth() * 0.5f;
            const float cy = p0.y + ImGui::GetWindowHeight() * 0.5f;
            if (fonts().metric) ImGui::PushFont(fonts().metric);
            text_centered(ImVec2(cx, cy - 20.0f), "Trial complete", theme::kGood);
            if (fonts().metric) ImGui::PopFont();
            text_centered(ImVec2(cx, cy + 18.0f),
                          "All rounds recorded and every survey answered.", theme::kMuted);
            ImGui::SetCursorScreenPos(ImVec2(cx - 90.0f, cy + 46.0f));
            if (ImGui::Button("New trial", ImVec2(180.0f, 38.0f))) {
                st.runner = TrialRunner{};
            }
        }
        end_card();
        return;
    }

    // ------------------------------------------------------------- running
    const PlannedRound& r = st.runner.current();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float main_w = (avail - gap) * 0.60f;
    const float side_w = (avail - gap) * 0.40f;
    const float row_h = 352.0f;

    if (begin_card("##run", ImVec2(main_w, row_h))) {
        eyebrow("Running");
        ImGui::SameLine();
        right_align(240.0f);
        char meta[64];
        std::snprintf(meta, sizeof(meta), "round %d of %d",
                      st.runner.round_index() + 1, st.runner.round_count());
        ImGui::TextColored(v4(theme::kMuted), "%s", meta);

        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float ring_r = 66.0f;
        const ImVec2 rc(o.x + ring_r + 8.0f, o.y + ring_r + 12.0f);

        const double left = std::max(0.0, st.runner.phase_remaining(st.durations));
        const double frac = st.runner.phase_elapsed() / st.durations.of(st.runner.phase());
        ring_gauge(rc, ring_r, 13.0f, frac,
                   st.runner.phase() == Phase::Stimulus ? theme::kAlpha : theme::kAccent,
                   theme::kGround);

        // Outer arc: progress through the whole trial, so the round countdown
        // never hides how much of the session is left.
        ring_gauge(rc, ring_r + 15.0f, 3.0f,
                   st.runner.trial_elapsed() / st.runner.trial_total(st.durations),
                   theme::kGood, theme::kLine);

        char cd[16];
        std::snprintf(cd, sizeof(cd), "%.0f", std::ceil(left));
        if (fonts().hero) ImGui::PushFont(fonts().hero);
        text_centered(ImVec2(rc.x, rc.y - 8.0f), cd, theme::kText);
        if (fonts().hero) ImGui::PopFont();
        if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
        text_centered(ImVec2(rc.x, rc.y + 26.0f), phase_label(st.runner.phase()),
                      theme::kMuted);
        if (fonts().eyebrow) ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(rc.x + ring_r + theme::kS5, o.y + 6.0f));
        ImGui::BeginGroup();
        if (fonts().hero) ImGui::PushFont(fonts().hero);
        if (r.cond == Condition::Stim) {
            if (st.design.mode == StimMode::Stacked) {
                ImGui::TextColored(v4(theme::kAlpha), "%d layers", st.design.enabled_count());
            } else {
                ImGui::TextColored(v4(theme::kAlpha), "%.1f Hz", r.hz);
            }
        } else {
            ImGui::TextColored(v4(condition_color(r.cond)), "control");
        }
        if (fonts().hero) ImGui::PopFont();

        ImGui::TextColored(v4(condition_color(r.cond)), "%s", condition_label(r.cond));
        ImGui::TextColored(v4(theme::kFaint), "carrier %.0f Hz - %.0f%% duty",
                           st.design.carrier_hz, st.design.duty * 100.0);
        ImGui::Dummy(ImVec2(1, theme::kS2));
        ImGui::TextColored(v4(st.runner.gate_open() ? theme::kAlpha : theme::kFaint),
                           st.runner.gate_open() ? "gate open - sounding"
                                                 : "gate closed - silence");
        ImGui::EndGroup();

        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + ring_r * 2.0f + theme::kS5));
        draw_phase_strip(st.runner, st.durations, ImGui::GetCursorScreenPos(),
                         ImGui::GetContentRegionAvail().x, 30.0f);
    }
    end_card();

    ImGui::SameLine(0.0f, gap);
    if (begin_card("##live", ImVec2(side_w, row_h))) {
        eyebrow("Live signal");
        ImGui::Dummy(ImVec2(1, theme::kS3));

        // Electrode state stays visible for the whole run, so a headset problem
        // is caught in the round it happens rather than at analysis.
        for (int i = 0; i < kSensorCount; ++i) {
            const auto& q = qual[static_cast<std::size_t>(i)];
            const theme::Rgba c = q.q == lsl::Quality::Good ? theme::kGood
                                : q.q == lsl::Quality::Fair ? theme::kWarn : theme::kBad;
            const int level = q.q == lsl::Quality::Good ? 0
                            : q.q == lsl::Quality::Fair ? 1 : 2;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            status_glyph(ImVec2(p.x + 10.0f, p.y + 11.0f), 9.0f, level, c);
            ImGui::SetCursorScreenPos(ImVec2(p.x + 28.0f, p.y));
            ImGui::TextColored(v4(theme::kDim), "%s",
                               electrode_name(static_cast<SensorId>(i)));
            ImGui::SameLine();
            right_align(70.0f);
            if (fonts().mono) ImGui::PushFont(fonts().mono);
            ImGui::TextColored(v4(c), "%.0f uV", q.rms_uv);
            if (fonts().mono) ImGui::PopFont();
            ImGui::Dummy(ImVec2(1, 6.0f));
        }

        ImGui::Dummy(ImVec2(1, theme::kS2));
        const double left_min = (st.runner.trial_total(st.durations) -
                                 st.runner.trial_elapsed()) / 60.0;
        ImGui::TextColored(v4(theme::kMuted), "about %.0f min remaining", left_min);
    }
    end_card();

    if (begin_card("##runsched", ImVec2(0, 168.0f))) {
        eyebrow("Round schedule");
        ImGui::SameLine();
        right_align(360.0f);
        ImGui::TextColored(v4(theme::kAlpha), "stimulus");
        ImGui::SameLine(0.0f, theme::kS3);
        ImGui::TextColored(v4(theme::kDelta), "jitter");
        ImGui::SameLine(0.0f, theme::kS3);
        ImGui::TextColored(v4(theme::kWarn), "tone");
        ImGui::Dummy(ImVec2(1, theme::kS2));
        draw_schedule(st.runner.schedule(), st.runner.round_index(),
                      ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);

        ImGui::Dummy(ImVec2(1, 34.0f));
        static const char* kSpeeds[] = {"1x", "8x", "30x"};
        static const double kSpeedVals[] = {1.0, 8.0, 30.0};
        int si = st.speed >= 30.0 ? 2 : st.speed >= 8.0 ? 1 : 0;
        if (segmented("##speed", kSpeeds, 3, &si, 150.0f)) st.speed = kSpeedVals[si];
        ImGui::SameLine(0.0f, theme::kS3);
        ImGui::TextColored(v4(theme::kFaint), "clock speed, for walking the flow");
        ImGui::SameLine();
        right_align(110.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, v4(theme::kBad));
        if (ImGui::Button("Abort trial", ImVec2(110.0f, 0))) st.runner.abort();
        ImGui::PopStyleColor();
    }
    end_card();

    if (st.survey_open) draw_survey_modal(st);
}

}  // namespace elanora::collector
