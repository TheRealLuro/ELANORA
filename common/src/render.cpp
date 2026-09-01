#include "elanora/render.hpp"

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
            if (glow) {
                dl->AddPolyline(run.data(), static_cast<int>(run.size()),
                                u32(color, 0.13f), ImDrawFlags_None, 4.0f);
            }
            dl->AddPolyline(run.data(), static_cast<int>(run.size()),
                            u32(color), ImDrawFlags_None, 1.0f);
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
    const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Border,
                                        ImGuiWindowFlags_NoScrollbar);
    if (open) {
        // Top-edge gradient: the card catches a little light at the top, which
        // separates stacked cards without adding another border line.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + 46.0f);
        dl->PushClipRect(p0, ImVec2(p1.x, p0.y + ImGui::GetWindowHeight()), true);
        dl->AddRectFilledMultiColor(p0, p1,
            u32(theme::kPanelHi), u32(theme::kPanelHi),
            u32(theme::kPanel),   u32(theme::kPanel));
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
    if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(theme::kMuted.r, theme::kMuted.g,
                                                theme::kMuted.b, 1.0f));
    // ImGui has no letter-spacing, so the effect is approximated by spacing
    // out an uppercase copy of the label.
    char buf[128];
    std::size_t o = 0;
    for (const char* p = text; *p && o + 3 < sizeof(buf); ++p) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
        buf[o++] = ch;
        if (p[1] != '\0') buf[o++] = ' ';
    }
    buf[o] = '\0';
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
    if (fonts().eyebrow) ImGui::PopFont();
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
