#include "elanora/render.hpp"

#include "elanora/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

#include "imgui.h"
#include "imgui_internal.h"

namespace elanora {

namespace {

ImU32 u32(theme::Rgba c, float alpha_mul = 1.0f) {
    return ImGui::GetColorU32(ImVec4{c.r, c.g, c.b, c.a * alpha_mul});
}

}  // namespace

// ---------------------------------------------------------------------------
// Display chain
// ---------------------------------------------------------------------------

void apply_highpass(std::vector<double>& v, int sampling_rate, double cutoff_hz) {
    if (v.size() < 2 || sampling_rate <= 0 || cutoff_hz <= 0) return;
    const double rc = 1.0 / (2.0 * 3.14159265358979323846 * cutoff_hz);
    const double a  = rc / (rc + 1.0 / static_cast<double>(sampling_rate));
    double prev_x = 0.0, prev_y = 0.0;
    bool primed = false;

    for (double& x : v) {
        if (std::isnan(x)) {
            // Reset state across a gap so the discontinuity cannot ring into
            // the samples that follow it.
            primed = false; prev_x = 0.0; prev_y = 0.0;
            continue;
        }
        if (!primed) { prev_x = x; prev_y = 0.0; primed = true; x = 0.0; continue; }
        const double y = a * (prev_y + x - prev_x);
        prev_x = x; prev_y = y; x = y;
    }
}

void apply_notch(std::vector<double>& v, int sampling_rate, double f0, double q) {
    if (v.size() < 3 || sampling_rate <= 0 || f0 <= 0 || q <= 0) return;
    const double w0    = 2.0 * 3.14159265358979323846 * f0 / sampling_rate;
    const double cosw  = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);

    double b0 = 1.0, b1 = -2.0 * cosw, b2 = 1.0;
    const double a0 = 1.0 + alpha, a1 = -2.0 * cosw, a2 = 1.0 - alpha;
    b0 /= a0; b1 /= a0; b2 /= a0;
    const double na1 = a1 / a0, na2 = a2 / a0;

    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (double& x : v) {
        if (std::isnan(x)) { x1 = x2 = y1 = y2 = 0.0; continue; }
        const double xin = x;
        const double y = b0 * xin + b1 * x1 + b2 * x2 - na1 * y1 - na2 * y2;
        x2 = x1; x1 = xin; y2 = y1; y1 = y;
        x = y;
    }
}

std::vector<double> display_chain(const std::vector<double>& raw,
                                  int sampling_rate,
                                  const DisplaySettings& s) {
    std::vector<double> out = raw;                 // never modify the record
    if (s.highpass) apply_highpass(out, sampling_rate, s.highpass_hz);
    if (s.notch)    apply_notch(out, sampling_rate, s.notch_hz, 12.0);
    return out;
}

// ---------------------------------------------------------------------------
// Trace rendering
// ---------------------------------------------------------------------------

int draw_trace_minmax(const std::vector<double>& samples,
                      ImVec2 origin, ImVec2 size,
                      double units_full_scale,
                      theme::Rgba color,
                      bool glow) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = static_cast<int>(samples.size());
    if (n <= 0 || size.x < 2.0f || size.y < 2.0f || units_full_scale <= 0.0) return 0;

    const int   cols   = std::max(1, static_cast<int>(size.x));
    const float mid    = origin.y + size.y * 0.5f;
    const float px_per = static_cast<float>((size.y * 0.5) / units_full_scale);

    int nan_count = 0;

    // Build runs of contiguous columns. A run ends wherever a column has no
    // finite sample, which is how a dropped packet becomes a visible gap.
    std::vector<ImVec2> run;
    run.reserve(static_cast<std::size_t>(cols) * 2u);

    auto flush = [&]() {
        if (run.size() >= 2) {
            // Fill from the centre line out to the envelope. A bare polyline
            // reads as a wireframe; the fill gives the trace body and makes
            // amplitude legible as area rather than only as excursion.
            if (glow) {
                for (std::size_t i = 0; i + 1 < run.size(); i += 2) {
                    dl->AddLine(ImVec2(run[i].x, mid), ImVec2(run[i].x, run[i].y),
                                u32(color, 0.07f), 1.0f);
                    dl->AddLine(ImVec2(run[i + 1].x, mid), ImVec2(run[i + 1].x, run[i + 1].y),
                                u32(color, 0.07f), 1.0f);
                }
                dl->AddPolyline(run.data(), static_cast<int>(run.size()),
                                u32(color, 0.10f), ImDrawFlags_None, 4.0f);
            }
            dl->AddPolyline(run.data(), static_cast<int>(run.size()),
                            u32(color), ImDrawFlags_None, 1.4f);
        }
        run.clear();
    };

    for (int px = 0; px < cols; ++px) {
        const int i0 = static_cast<int>(static_cast<long long>(px) * n / cols);
        const int i1 = std::max(i0 + 1,
                       static_cast<int>(static_cast<long long>(px + 1) * n / cols));

        double mn =  std::numeric_limits<double>::infinity();
        double mx = -std::numeric_limits<double>::infinity();
        bool any = false;
        for (int i = i0; i < i1 && i < n; ++i) {
            const double v = samples[static_cast<std::size_t>(i)];
            if (std::isnan(v)) { ++nan_count; continue; }
            any = true;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        if (!any) { flush(); continue; }

        const float x  = origin.x + static_cast<float>(px);
        const float y0 = mid - static_cast<float>(mx) * px_per;
        const float y1 = mid - static_cast<float>(mn) * px_per;
        const float top = std::clamp(y0, origin.y, origin.y + size.y);
        const float bot = std::clamp(y1, origin.y, origin.y + size.y);
        run.push_back(ImVec2(x, top));
        run.push_back(ImVec2(x, bot));
    }
    flush();
    return nan_count;
}

void draw_time_grid(ImVec2 origin, ImVec2 size, double span_seconds, double div_seconds) {
    if (span_seconds <= 0.0 || div_seconds <= 0.0 || size.x < 4.0f) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int divs = static_cast<int>(span_seconds / div_seconds);

    for (int d = 0; d <= divs; ++d) {
        const double t = static_cast<double>(d) * div_seconds;
        const float x = origin.x + static_cast<float>((t / span_seconds)) * size.x;
        // "Now" is the right edge; ticks count backwards from it.
        // Every 2 s. Every 5 left a single label on an 8 s window, which is
        // not enough reference to read an interval off.
        const bool major = (divs - d) % 2 == 0;
        dl->AddLine(ImVec2(std::round(x) + 0.5f, origin.y),
                    ImVec2(std::round(x) + 0.5f, origin.y + size.y),
                    u32(theme::kLine, major ? 0.85f : 0.35f), 1.0f);

        if (major && d < divs) {
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "-%.0fs", span_seconds - t);
            if (fonts().mono) ImGui::PushFont(fonts().mono);
            // Below the plot body, never inside it: a label drawn over the
            // bottom lane is unreadable and hides signal at the same time.
            dl->AddText(ImVec2(x + 3.0f, origin.y + size.y + 2.0f),
                        u32(theme::kFaint), lbl);
            if (fonts().mono) ImGui::PopFont();
        }
    }
}

void draw_scale_bar(ImVec2 origin, float lane_height, double uv_per_div,
                    theme::Rgba color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // One division is half a lane, matching draw_trace_minmax's full-scale of
    // two divisions per lane.
    const float h = lane_height * 0.5f;
    const float x = origin.x + 6.0f;
    const float y0 = origin.y + lane_height * 0.5f - h * 0.5f;

    dl->AddLine(ImVec2(x, y0), ImVec2(x, y0 + h), u32(color, 0.8f), 1.5f);
    dl->AddLine(ImVec2(x - 3.0f, y0), ImVec2(x + 3.0f, y0), u32(color, 0.8f), 1.5f);
    dl->AddLine(ImVec2(x - 3.0f, y0 + h), ImVec2(x + 3.0f, y0 + h), u32(color, 0.8f), 1.5f);

    (void)uv_per_div;   // the caption under the plot states the sensitivity
}

void draw_spectrum(const double* mags, int n_bins, double bin_hz,
                   ImVec2 origin, ImVec2 size, double f_max,
                   double* peak_hz_out) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (mags == nullptr || n_bins <= 2 || size.x < 8.0f || f_max <= 0.0) return;

    const float label_h = ImGui::GetTextLineHeight() + 2.0f;
    const float plot_h = size.y - label_h;

    // Band regions behind the curve.
    for (int b = 0; b < 5; ++b) {
        const double lo = kBandEdges[static_cast<std::size_t>(b)][0];
        const double hi = kBandEdges[static_cast<std::size_t>(b)][1];
        const float x0 = origin.x + static_cast<float>(lo / f_max) * size.x;
        const float x1 = origin.x + static_cast<float>(std::min(hi, f_max) / f_max) * size.x;
        const auto col = theme::band_color(b);
        dl->AddLine(ImVec2(std::round(x0) + 0.5f, origin.y),
                    ImVec2(std::round(x0) + 0.5f, origin.y + plot_h), u32(theme::kLine, 0.9f));
        // A short coloured tick at the baseline, rather than a wash of colour
        // across the whole region.
        dl->AddLine(ImVec2(std::round(x0) + 0.5f, origin.y + plot_h - 5.0f),
                    ImVec2(std::round(x0) + 0.5f, origin.y + plot_h), u32(col, 0.9f), 2.0f);
        if (fonts().mono) ImGui::PushFont(fonts().mono);
        const char* nm = band_name(static_cast<Band>(b));
        if (ImGui::CalcTextSize(nm).x + 6.0f < (x1 - x0)) {
            dl->AddText(ImVec2(x0 + 3.0f, origin.y + plot_h + 1.0f), u32(col, 0.75f), nm);
        }
        if (fonts().mono) ImGui::PopFont();
    }

    // dB, because the 1/f slope of real EEG is a straight line in log and an
    // uninformative cliff in linear.
    const int last = std::min(n_bins - 1, static_cast<int>(f_max / bin_hz));
    if (last < 2) return;
    double lo_db = 1e30, hi_db = -1e30;
    std::vector<double> db(static_cast<std::size_t>(last));
    for (int k = 1; k <= last; ++k) {
        const double v = 20.0 * std::log10(mags[k] + 1e-12);
        db[static_cast<std::size_t>(k - 1)] = v;
        lo_db = std::min(lo_db, v);
        hi_db = std::max(hi_db, v);
    }
    const double range = std::max(hi_db - lo_db, 1.0);

    std::vector<ImVec2> pts;
    pts.reserve(static_cast<std::size_t>(last) + 2u);
    int peak_k = 1; double peak_v = -1e30;
    for (int k = 1; k <= last; ++k) {
        const double v = db[static_cast<std::size_t>(k - 1)];
        // Ignore the lowest bins when hunting the peak: 1/f always wins there
        // and would report "peak 1 Hz" on every recording ever made.
        if (k * bin_hz >= 4.0 && v > peak_v) { peak_v = v; peak_k = k; }
        const float x = origin.x + static_cast<float>((k * bin_hz) / f_max) * size.x;
        const float y = origin.y + plot_h -
                        static_cast<float>((v - lo_db) / range) * (plot_h - 4.0f) - 2.0f;
        pts.push_back(ImVec2(x, y));
    }
    if (peak_hz_out) *peak_hz_out = peak_k * bin_hz;

    // Area fill drawn as one vertical line per point. AddConvexPolyFilled was
    // wrong here and looked it: a spectrum curve is not convex, and feeding a
    // concave outline to it produces spurious triangles fanning across the plot.
    for (std::size_t i = 0; i < pts.size(); ++i) {
        dl->AddLine(ImVec2(pts[i].x, pts[i].y),
                    ImVec2(pts[i].x, origin.y + plot_h),
                    u32(theme::kAccent, 0.11f), 1.6f);
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                    u32(theme::kAccent, 0.16f), ImDrawFlags_None, 4.0f);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                    u32(theme::kAccent), ImDrawFlags_None, 1.4f);

    // Peak marker.
    const float px = origin.x + static_cast<float>((peak_k * bin_hz) / f_max) * size.x;
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + plot_h), u32(theme::kText, 0.28f));
    dl->AddCircleFilled(ImVec2(px, pts[static_cast<std::size_t>(peak_k - 1)].y), 2.5f,
                        u32(theme::kText, 0.9f));
}

void band_cell(ImVec2 pos, ImVec2 size, double value, theme::Rgba color, bool dominant) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float v = static_cast<float>(std::clamp(value, 0.0, 1.0));
    (void)color;   // hue identifies the band in the header, not in every cell

    // A bar, not a filled box.
    //
    // Five hues x twenty cells produced a grid of muddy coloured rectangles
    // that read as bad spreadsheet conditional formatting, and the dark low-
    // value fills looked disabled rather than small. Length is a far more
    // precise visual encoding than opacity, and dropping to one neutral hue
    // removes nineteen competing colours from the panel.
    const float track_h = 3.0f;
    const float track_y = pos.y + size.y - track_h;

    dl->AddRectFilled(ImVec2(pos.x, track_y),
                      ImVec2(pos.x + size.x, track_y + track_h),
                      u32(theme::kLine), track_h * 0.5f);
    if (v > 0.001f) {
        dl->AddRectFilled(ImVec2(pos.x, track_y),
                          ImVec2(pos.x + size.x * v, track_y + track_h),
                          u32(dominant ? theme::kAccent : theme::kMuted, dominant ? 1.0f : 0.55f),
                          track_h * 0.5f);
    }

    char lbl[8];
    std::snprintf(lbl, sizeof(lbl), "%.2f", value);
    // Value in the UI face, not mono: mono on every number is a terminal look.
    const ImVec2 ts = ImGui::CalcTextSize(lbl);
    dl->AddText(ImVec2(pos.x, pos.y + (size.y - track_h - ts.y) * 0.5f),
                u32(dominant ? theme::kText : theme::kMuted, dominant ? 1.0f : 0.8f), lbl);
    (void)ts;
}

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

double smooth_to(double current, double target, float dt, double tau) {
    if (tau <= 0.0 || dt <= 0.0f) return target;
    // Exponential approach. Using exp() rather than a fixed per-frame fraction
    // keeps the settling time identical at 60 Hz and at 144 Hz.
    const double a = 1.0 - std::exp(-static_cast<double>(dt) / tau);
    const double next = current + (target - current) * a;
    // Snap once the remaining distance is invisible, so values settle exactly
    // rather than creeping forever and re-rendering every frame.
    return (std::abs(target - next) < 1e-4) ? target : next;
}

theme::Rgba lerp_color(theme::Rgba a, theme::Rgba b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return theme::Rgba{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

bool segmented(const char* id, const char* const* labels, int count,
               int* current, float width) {
    if (count <= 0 || current == nullptr) return false;

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID base = win->GetID(id);

    const float pad = 3.0f;
    const float h = ImGui::GetFontSize() + pad * 2.0f + 6.0f;
    float w = width;
    if (w <= 0.0f) {
        w = pad * 2.0f;
        for (int i = 0; i < count; ++i) w += ImGui::CalcTextSize(labels[i]).x + 24.0f;
    }
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float seg_w = (w - pad * 2.0f) / static_cast<float>(count);

    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemActive() || ImGui::IsItemClicked()) {
        const float local = ImGui::GetIO().MousePos.x - pos.x - pad;
        const int idx = std::clamp(static_cast<int>(local / seg_w), 0, count - 1);
        if (ImGui::IsItemClicked() && idx != *current) { *current = idx; changed = true; }
    }

    // The indicator eases toward the selected segment, so the control shows
    // which direction the selection moved rather than teleporting.
    const ImGuiID anim_id = base + 1;
    float anim = store->GetFloat(anim_id, static_cast<float>(*current));
    anim = static_cast<float>(smooth_to(anim, static_cast<double>(*current),
                                        ImGui::GetIO().DeltaTime, 0.09));
    store->SetFloat(anim_id, anim);

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(theme::kRaised), h * 0.5f);

    const float ix = pos.x + pad + anim * seg_w;
    dl->AddRectFilled(ImVec2(ix, pos.y + pad), ImVec2(ix + seg_w, pos.y + h - pad),
                      u32(theme::kAccent, hovered ? 1.0f : 0.92f), (h - pad * 2.0f) * 0.5f);

    for (int i = 0; i < count; ++i) {
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const float cx = pos.x + pad + seg_w * (static_cast<float>(i) + 0.5f) - ts.x * 0.5f;
        // Fade the label between muted and the on-accent colour as the
        // indicator passes over it, rather than switching at the midpoint.
        const float on = std::clamp(1.0f - std::abs(anim - static_cast<float>(i)), 0.0f, 1.0f);
        dl->AddText(ImVec2(cx, pos.y + (h - ts.y) * 0.5f),
                    ImGui::GetColorU32(ImVec4(
                        lerp_color(theme::kMuted, theme::kGround, on).r,
                        lerp_color(theme::kMuted, theme::kGround, on).g,
                        lerp_color(theme::kMuted, theme::kGround, on).b, 1.0f)),
                    labels[i]);
    }
    return changed;
}

bool toggle_switch(const char* id, bool* value) {
    if (value == nullptr) return false;

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiStorage* store = ImGui::GetStateStorage();
    const ImGuiID anim_id = win->GetID(id) + 1;

    const float h = ImGui::GetFontSize() + 4.0f;
    const float w = h * 1.85f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *value = !*value; changed = true; }
    const bool hovered = ImGui::IsItemHovered();

    float anim = store->GetFloat(anim_id, *value ? 1.0f : 0.0f);
    anim = static_cast<float>(smooth_to(anim, *value ? 1.0 : 0.0,
                                        ImGui::GetIO().DeltaTime, 0.08));
    store->SetFloat(anim_id, anim);

    const theme::Rgba track = lerp_color(theme::kLineHi, theme::kAccent, anim);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      u32(track, hovered ? 1.0f : 0.9f), h * 0.5f);

    const float r = h * 0.5f - 2.5f;
    const float kx = pos.x + 2.5f + r + anim * (w - 2.0f * (r + 2.5f));
    dl->AddCircleFilled(ImVec2(kx, pos.y + h * 0.5f), r,
                        u32(anim > 0.5f ? theme::kGround : theme::kDim));
    return changed;
}

// ---------------------------------------------------------------------------
// Card chrome
// ---------------------------------------------------------------------------

bool begin_card(const char* id, ImVec2 size) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(theme::kPanel.r, theme::kPanel.g,
                                                   theme::kPanel.b, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(theme::kLine.r, theme::kLine.g,
                                                   theme::kLine.b, 1.0f));
    // No border flag: the panel is separated from the ground by fill contrast
    // and spacing alone. An outline around every card is chrome competing with
    // the content it is supposed to frame.
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_None,
                                        ImGuiWindowFlags_NoScrollbar);
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        const float h = ImGui::GetWindowHeight();
        dl->PushClipRect(p0, ImVec2(p0.x + w, p0.y + h), true);

        // Top-edge gradient: the card catches a little light at the top.
        dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + w, p0.y + 52.0f),
            u32(theme::kPanelHi), u32(theme::kPanelHi),
            u32(theme::kPanel),   u32(theme::kPanel));

        // A one-pixel lit edge along the top. This is what makes a panel read
        // as a raised plane rather than a lighter rectangle -- the same trick
        // a bevel uses, at a thousandth of the visual cost.
        dl->AddLine(ImVec2(p0.x + theme::kRadius, p0.y + 0.5f),
                    ImVec2(p0.x + w - theme::kRadius, p0.y + 0.5f),
                    u32(theme::kLineHi, 0.55f), 1.0f);
        dl->PopClipRect();
    }
    return open;
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void eyebrow(const char* text) {
    // Real letter-spacing, drawn glyph by glyph with a small extra advance.
    //
    // The previous version injected a space character between every letter,
    // which is roughly 0.5em of tracking where the intent was 0.08em. It read
    // as "E E G" and was the most conspicuously amateur thing on screen.
    if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImU32 col = u32(theme::kMuted);
    const float tracking = ImGui::GetFontSize() * 0.09f;

    float x = start.x;
    for (const char* p = text; *p != '\0'; ++p) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        const char one[2] = {ch, '\0'};
        dl->AddText(ImVec2(x, start.y), col, one);
        x += ImGui::CalcTextSize(one).x + tracking;
    }
    const float h = ImGui::GetTextLineHeight();
    if (fonts().eyebrow) ImGui::PopFont();
    ImGui::Dummy(ImVec2(x - start.x, h));
}

void readout(const char* value, const char* unit, theme::Rgba color) {
    if (fonts().display) ImGui::PushFont(fonts().display);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r, color.g, color.b, color.a));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    if (fonts().display) ImGui::PopFont();

    if (unit != nullptr && unit[0] != '\0') {
        ImGui::SameLine(0.0f, theme::kS2);
        // Sit the unit on the value's baseline rather than its top edge.
        const float shift = ImGui::GetTextLineHeight() * 0.35f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + shift);
        if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
        ImGui::TextColored(ImVec4(theme::kMuted.r, theme::kMuted.g, theme::kMuted.b, 1.0f),
                           "%s", unit);
        if (fonts().eyebrow) ImGui::PopFont();
    }
}

void status_pill(const char* text, theme::Rgba color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 ts  = ImGui::CalcTextSize(text);
    const float pad_x = 11.0f, pad_y = 4.0f, dot_r = 3.5f, gap = 7.0f;
    const float w = pad_x * 2 + dot_r * 2 + gap + ts.x;
    const float h = ts.y + pad_y * 2;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(theme::kRaised), h * 0.5f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), u32(color, 0.35f), h * 0.5f);
    dl->AddCircleFilled(ImVec2(pos.x + pad_x + dot_r, pos.y + h * 0.5f), dot_r, u32(color));
    dl->AddText(ImVec2(pos.x + pad_x + dot_r * 2 + gap, pos.y + pad_y), u32(color), text);

    ImGui::Dummy(ImVec2(w, h));
}

void right_align(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SameLine(0.0f, avail - width);
}

void text_colored(theme::Rgba c, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void text_mono(theme::Rgba c, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (fonts().mono) ImGui::PushFont(fonts().mono);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    if (fonts().mono) ImGui::PopFont();
    va_end(args);
}

}  // namespace elanora
