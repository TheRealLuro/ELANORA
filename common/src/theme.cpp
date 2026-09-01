#include "elanora/theme.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "implot.h"

namespace elanora {

namespace {

constexpr ImVec4 v4(theme::Rgba c) { return ImVec4{c.r, c.g, c.b, c.a}; }

// Slightly lifted variants used for hover/active states, derived from the
// palette rather than hand-picked so they stay consistent if it changes.
ImVec4 lift(theme::Rgba c, float amount) {
    return ImVec4{c.r + amount, c.g + amount, c.b + amount, c.a};
}

Fonts g_fonts;

namespace fs = std::filesystem;

// Search order for each role. Bundled Fira first because it is the pairing the
// design system selected for data dashboards; then the Windows system faces,
// which are always present and still far better than the ImGui default.
struct FontCandidates {
    std::vector<std::string> paths;
};

FontCandidates ui_regular() {
    return {{ "common/assets/fonts/FiraSans-Regular.ttf",
              "assets/fonts/FiraSans-Regular.ttf",
              "C:/Windows/Fonts/segoeui.ttf" }};
}
FontCandidates ui_medium() {
    return {{ "common/assets/fonts/FiraSans-Medium.ttf",
              "assets/fonts/FiraSans-Medium.ttf",
              "C:/Windows/Fonts/segoeuisb.ttf",
              "C:/Windows/Fonts/segoeui.ttf" }};
}
FontCandidates ui_light() {
    return {{ "common/assets/fonts/FiraSans-Light.ttf",
              "assets/fonts/FiraSans-Light.ttf",
              "C:/Windows/Fonts/segoeuil.ttf",
              "C:/Windows/Fonts/segoeui.ttf" }};
}
FontCandidates mono_regular() {
    return {{ "common/assets/fonts/FiraMono-Regular.ttf",
              "assets/fonts/FiraMono-Regular.ttf",
              "C:/Windows/Fonts/consola.ttf" }};
}

// Returns nullptr when nothing loadable is found; the caller then falls back
// to whatever ImGui already has, so a missing font is never fatal.
ImFont* load_first(const FontCandidates& c, float size_px, bool* was_bundled) {
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& p : c.paths) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;

        // A failed download can leave a zero-byte or truncated file on disk,
        // and AddFontFromFileTTF asserts rather than returning null on one.
        // Anything smaller than this is not a usable font.
        const auto sz = fs::file_size(p, ec);
        if (ec || sz < 4096) continue;

        ImFont* f = io.Fonts->AddFontFromFileTTF(p.c_str(), size_px);
        if (f != nullptr) {
            if (was_bundled != nullptr && p.find("Fira") != std::string::npos) {
                *was_bundled = true;
            }
            return f;
        }
    }
    return nullptr;
}

}  // namespace

Fonts& load_fonts() {
    ImGuiIO& io = ImGui::GetIO();

    // Body first: whichever font is added first becomes ImGui's default, and
    // the default is what every un-pushed widget renders with.
    g_fonts.body    = load_first(ui_regular(),   15.0f, &g_fonts.bundled);
    g_fonts.subhead = load_first(ui_medium(),    17.0f, &g_fonts.bundled);
    g_fonts.display = load_first(ui_light(),     38.0f, &g_fonts.bundled);
    g_fonts.mono    = load_first(mono_regular(), 13.0f, &g_fonts.bundled);
    g_fonts.eyebrow = load_first(ui_medium(),    11.0f, &g_fonts.bundled);

    if (g_fonts.body == nullptr) {
        // No file-backed face anywhere. ProggyClean it is -- ugly, but the app
        // still runs, which matters more than the typography.
        g_fonts.body = io.Fonts->AddFontDefault();
    }
    if (g_fonts.subhead == nullptr) g_fonts.subhead = g_fonts.body;
    if (g_fonts.display == nullptr) g_fonts.display = g_fonts.body;
    if (g_fonts.mono    == nullptr) g_fonts.mono    = g_fonts.body;
    if (g_fonts.eyebrow == nullptr) g_fonts.eyebrow = g_fonts.body;

    io.Fonts->Build();
    return g_fonts;
}

const Fonts& fonts() { return g_fonts; }

namespace theme {

const Rgba& band_color(int band_index) {
    static const Rgba table[5] = { kDelta, kTheta, kAlpha, kBeta, kGamma };
    if (band_index < 0 || band_index > 4) return kMuted;
    return table[band_index];
}

}  // namespace theme

void apply_elanora_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    // Dense-dashboard metrics: tight padding, generous item spacing so rows
    // stay scannable, rounded but not soft.
    s.WindowRounding    = theme::kRadius;
    s.ChildRounding     = theme::kRadius;
    s.FrameRounding     = theme::kRadiusSm;
    s.PopupRounding     = theme::kRadius;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = theme::kRadiusSm;
    s.TabRounding       = theme::kRadiusSm;

    s.WindowPadding    = ImVec2(theme::kS5, theme::kS4);
    s.FramePadding     = ImVec2(theme::kS3, 7.0f);
    s.ItemSpacing      = ImVec2(theme::kS3, theme::kS3);
    s.ItemInnerSpacing = ImVec2(theme::kS2, 6.0f);
    s.CellPadding      = ImVec2(theme::kS2, 5.0f);

    // Borders mostly removed. Separation comes from fill contrast and space;
    // a 1px line around every panel is chrome competing with the content.
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize  = 0.0f;
    s.FrameBorderSize  = 0.0f;
    s.ScrollbarSize    = 11.0f;
    s.GrabMinSize      = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]         = v4(theme::kGround);
    c[ImGuiCol_ChildBg]          = v4(theme::kPanel);
    c[ImGuiCol_PopupBg]          = v4(theme::kPanel);
    c[ImGuiCol_Border]           = v4(theme::kLine);
    c[ImGuiCol_BorderShadow]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]             = v4(theme::kText);
    c[ImGuiCol_TextDisabled]     = v4(theme::kMuted);

    c[ImGuiCol_FrameBg]          = v4(theme::kRaised);
    c[ImGuiCol_FrameBgHovered]   = lift(theme::kRaised, 0.035f);
    c[ImGuiCol_FrameBgActive]    = lift(theme::kRaised, 0.06f);

    c[ImGuiCol_TitleBg]          = v4(theme::kPanel);
    c[ImGuiCol_TitleBgActive]    = v4(theme::kPanelHi);
    c[ImGuiCol_TitleBgCollapsed] = v4(theme::kGround);

    c[ImGuiCol_Header]           = v4(theme::kRaised);
    c[ImGuiCol_HeaderHovered]    = lift(theme::kRaised, 0.035f);
    c[ImGuiCol_HeaderActive]     = lift(theme::kRaised, 0.06f);

    c[ImGuiCol_Button]           = v4(theme::kRaised);
    c[ImGuiCol_ButtonHovered]    = lift(theme::kRaised, 0.045f);
    c[ImGuiCol_ButtonActive]     = v4(theme::kAccent);

    c[ImGuiCol_SliderGrab]       = v4(theme::kAccent);
    c[ImGuiCol_SliderGrabActive] = v4(theme::kAccent);
    c[ImGuiCol_CheckMark]        = v4(theme::kAccent);

    c[ImGuiCol_Tab]              = v4(theme::kPanel);
    c[ImGuiCol_TabHovered]       = lift(theme::kRaised, 0.045f);
    c[ImGuiCol_TabActive]        = v4(theme::kPanelHi);

    c[ImGuiCol_Separator]        = v4(theme::kLine);
    c[ImGuiCol_SeparatorHovered] = v4(theme::kLineHi);
    c[ImGuiCol_TableHeaderBg]    = v4(theme::kRaised);
    c[ImGuiCol_TableBorderStrong]= v4(theme::kLine);
    c[ImGuiCol_TableBorderLight] = v4(theme::kPanel);
    c[ImGuiCol_TableRowBgAlt]    = ImVec4(1.0f, 1.0f, 1.0f, 0.015f);

    c[ImGuiCol_PlotLines]        = v4(theme::kAccent);
    c[ImGuiCol_PlotHistogram]    = v4(theme::kAccent);
    c[ImGuiCol_ScrollbarBg]      = v4(theme::kPanel);
    c[ImGuiCol_ScrollbarGrab]    = v4(theme::kLine);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(theme::kLineHi);

    // ImPlot must match, or every chart looks pasted in from another program.
    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.PlotPadding    = ImVec2(theme::kS3, theme::kS2);
    ps.LabelPadding   = ImVec2(5, 4);
    ps.LegendPadding  = ImVec2(theme::kS2, theme::kS2);
    ps.PlotBorderSize = 0.0f;   // the card border already frames the plot
    ps.MinorAlpha     = 0.18f;
    ps.LineWeight     = 1.2f;

    ImVec4* pc = ps.Colors;
    pc[ImPlotCol_FrameBg]      = ImVec4(0, 0, 0, 0);
    pc[ImPlotCol_PlotBg]       = ImVec4(0, 0, 0, 0);
    pc[ImPlotCol_PlotBorder]   = ImVec4(0, 0, 0, 0);
    pc[ImPlotCol_LegendBg]     = v4(theme::kRaised);
    pc[ImPlotCol_LegendBorder] = v4(theme::kLine);
    pc[ImPlotCol_LegendText]   = v4(theme::kDim);
    pc[ImPlotCol_TitleText]    = v4(theme::kText);
    pc[ImPlotCol_InlayText]    = v4(theme::kMuted);
    pc[ImPlotCol_AxisText]     = v4(theme::kFaint);
    pc[ImPlotCol_AxisGrid]     = ImVec4(theme::kLine.r, theme::kLine.g, theme::kLine.b, 0.55f);
    pc[ImPlotCol_AxisTick]     = v4(theme::kLine);
}

}  // namespace elanora
