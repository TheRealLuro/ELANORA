#include "elanora/theme.hpp"

#include "imgui.h"
#include "implot.h"

namespace elanora {

namespace {

constexpr ImVec4 rgba(theme::Rgba c) { return ImVec4{c.r, c.g, c.b, c.a}; }

// Neutral dark greys. Deliberately low-chroma so the semantic colours in
// theme.hpp (quality lamps, band traces) are the only saturated things on
// screen and therefore the things the eye goes to first.
constexpr ImVec4 kBg      {0.06f, 0.07f, 0.09f, 1.00f};
constexpr ImVec4 kPanel   {0.10f, 0.11f, 0.14f, 1.00f};
constexpr ImVec4 kRaised  {0.14f, 0.15f, 0.19f, 1.00f};
constexpr ImVec4 kHover   {0.19f, 0.21f, 0.26f, 1.00f};
constexpr ImVec4 kActive  {0.24f, 0.26f, 0.32f, 1.00f};
constexpr ImVec4 kBorder  {0.22f, 0.24f, 0.29f, 1.00f};
constexpr ImVec4 kText    {0.90f, 0.91f, 0.93f, 1.00f};
constexpr ImVec4 kTextDim {0.55f, 0.58f, 0.64f, 1.00f};

}  // namespace

void apply_elanora_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    // Metrics. Generous padding because these are instrument panels read at a
    // glance during a running trial, not dense developer tooling.
    s.WindowRounding    = 6.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 5.0f;
    s.TabRounding       = 5.0f;

    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.CellPadding       = ImVec2(8, 5);

    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]            = kBg;
    c[ImGuiCol_ChildBg]             = kPanel;
    c[ImGuiCol_PopupBg]             = kPanel;
    c[ImGuiCol_Border]              = kBorder;
    c[ImGuiCol_Text]                = kText;
    c[ImGuiCol_TextDisabled]        = kTextDim;

    c[ImGuiCol_FrameBg]             = kRaised;
    c[ImGuiCol_FrameBgHovered]      = kHover;
    c[ImGuiCol_FrameBgActive]       = kActive;

    c[ImGuiCol_TitleBg]             = kPanel;
    c[ImGuiCol_TitleBgActive]       = kRaised;
    c[ImGuiCol_TitleBgCollapsed]    = kBg;

    c[ImGuiCol_Header]              = kRaised;
    c[ImGuiCol_HeaderHovered]       = kHover;
    c[ImGuiCol_HeaderActive]        = kActive;

    c[ImGuiCol_Button]              = kRaised;
    c[ImGuiCol_ButtonHovered]       = kHover;
    c[ImGuiCol_ButtonActive]        = rgba(theme::kAccent);

    c[ImGuiCol_SliderGrab]          = rgba(theme::kAccent);
    c[ImGuiCol_SliderGrabActive]    = rgba(theme::kAccent);
    c[ImGuiCol_CheckMark]           = rgba(theme::kAccent);

    c[ImGuiCol_Tab]                 = kPanel;
    c[ImGuiCol_TabHovered]          = kHover;
    c[ImGuiCol_TabActive]         = kRaised;

    c[ImGuiCol_Separator]           = kBorder;
    c[ImGuiCol_TableHeaderBg]       = kRaised;
    c[ImGuiCol_TableBorderStrong]   = kBorder;
    c[ImGuiCol_TableBorderLight]    = kPanel;
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);

    c[ImGuiCol_PlotLines]           = rgba(theme::kAccent);
    c[ImGuiCol_PlotHistogram]       = rgba(theme::kAccent);

    // ImPlot must match, or every chart looks pasted in from another program.
    ImPlotStyle& ps = ImPlot::GetStyle();
    ps.PlotPadding      = ImVec2(12, 10);
    ps.LabelPadding     = ImVec2(6, 5);
    ps.LegendPadding    = ImVec2(8, 8);
    ps.PlotBorderSize   = 1.0f;
    ps.MinorAlpha       = 0.20f;

    ImVec4* pc = ps.Colors;
    pc[ImPlotCol_FrameBg]     = kPanel;
    pc[ImPlotCol_PlotBg]      = kBg;
    pc[ImPlotCol_PlotBorder]  = kBorder;
    pc[ImPlotCol_LegendBg]    = kPanel;
    pc[ImPlotCol_LegendBorder]= kBorder;
    pc[ImPlotCol_LegendText]  = kText;
    pc[ImPlotCol_TitleText]   = kText;
    pc[ImPlotCol_InlayText]   = kTextDim;
    pc[ImPlotCol_AxisText]    = kTextDim;
    pc[ImPlotCol_AxisGrid]    = kBorder;
}

}  // namespace elanora
