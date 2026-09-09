// UI smoke check.
//
// Opens a real window, renders a few frames through ImGui and ImPlot, then
// exits. Verifies the parts of the GUI stack no unit test can reach: GLFW
// window creation, the GL context, backend init, and clean teardown.
//
//   ui_smoke            open and wait for the user to close it
//   ui_smoke --frames N render N frames then exit 0 (for automation)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "elanora/theme.hpp"
#include "elanora/types.hpp"
#include "elanora/ui_shell.hpp"

#include "imgui.h"
#include "implot.h"

using namespace elanora;

int main(int argc, char** argv) {
    int max_frames = -1;  // negative means run until closed
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        }
    }

    UiShell shell("ELANORA UI smoke", 900, 600);
    if (!shell.ok()) {
        std::fprintf(stderr, "UiShell failed: %s\n", shell.error().c_str());
        return 1;
    }

    // A sine per band, so the theme's band colours are exercised too.
    std::vector<double> xs(200), ys(200);
    for (int i = 0; i < 200; ++i) {
        xs[i] = i * 0.05;
        ys[i] = std::sin(xs[i]);
    }

    int frames = 0;
    while (shell.begin_frame()) {
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(560, 420), ImGuiCond_Once);
        ImGui::Begin("Smoke");

        ImGui::TextUnformatted("ELANORA shared UI shell");
        ImGui::Separator();

        for (int b = 0; b < kBandCount; ++b) {
            const auto band = static_cast<Band>(b);
            ImGui::Text("%-6s", band_name(band));
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f-%.1f Hz", kBandEdges[b][0], kBandEdges[b][1]);
        }

        ImGui::Separator();
        if (ImPlot::BeginPlot("signal", ImVec2(-1, 200))) {
            ImPlot::PlotLine("sine", xs.data(), ys.data(), (int)xs.size());
            ImPlot::EndPlot();
        }

        ImGui::End();
        shell.end_frame();

        if (max_frames >= 0 && ++frames >= max_frames) break;
    }

    std::printf("ui_smoke: rendered %d frames, shutting down cleanly\n", frames);
    return 0;
}
