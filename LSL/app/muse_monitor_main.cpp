// muse_monitor -- live Muse 2 signal viewer.
//
// The gate for everything downstream: if the four EEG traces are not moving,
// the PPG pulse is not visible, and the IMU does not respond to head motion,
// then no dataset collected afterwards means anything.
//
//   muse_monitor                 connect to the first Muse 2 found
//   muse_monitor --synthetic     run against BrainFlow's synthetic board
//   muse_monitor --frames N      render N frames then exit (automation)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "elanora/lsl/muse_device.hpp"
#include "elanora/lsl/signal_quality.hpp"
#include "elanora/lsl/stream_recorder.hpp"
#include "elanora/theme.hpp"
#include "elanora/types.hpp"
#include "elanora/ui_shell.hpp"

#include "imgui.h"
#include "implot.h"

using namespace elanora;
using namespace elanora::lsl;

namespace {

constexpr double kPlotSeconds = 8.0;

ImVec4 to_imvec(theme::Rgba c) { return ImVec4{c.r, c.g, c.b, c.a}; }

ImVec4 quality_color(Quality q) {
    switch (q) {
        case Quality::Good: return to_imvec(theme::kGood);
        case Quality::Fair: return to_imvec(theme::kFair);
        case Quality::Bad:  return to_imvec(theme::kBad);
    }
    return to_imvec(theme::kMuted);
}

// One channel pulled out of a window as (relative seconds, value) pairs.
struct Trace {
    std::vector<double> t;
    std::vector<double> v;
};

Trace extract(const std::vector<Sample>& win, int row, double t_end) {
    Trace tr;
    if (row < 0) return tr;
    tr.t.reserve(win.size());
    tr.v.reserve(win.size());
    for (const auto& s : win) {
        if (row >= static_cast<int>(s.values.size())) continue;
        tr.t.push_back(s.ts - t_end);  // negative seconds, 0 == now
        tr.v.push_back(s.values[static_cast<std::size_t>(row)]);
    }
    return tr;
}

void plot_traces(const char* title, const std::vector<Sample>& win, double t_end,
                 const std::vector<int>& rows, const char* const* labels,
                 const theme::Rgba* colors, int n_labels, float height) {
    if (!ImPlot::BeginPlot(title, ImVec2(-1, height))) return;
    ImPlot::SetupAxes("seconds", nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit);
    ImPlot::SetupAxisLimits(ImAxis_X1, -kPlotSeconds, 0.0, ImPlotCond_Always);

    for (std::size_t i = 0; i < rows.size() && i < static_cast<std::size_t>(n_labels); ++i) {
        const Trace tr = extract(win, rows[i], t_end);
        if (tr.t.empty()) continue;
        if (colors != nullptr) ImPlot::SetNextLineStyle(to_imvec(colors[i]));
        ImPlot::PlotLine(labels[i], tr.t.data(), tr.v.data(), static_cast<int>(tr.t.size()));
    }
    ImPlot::EndPlot();
}

}  // namespace

int main(int argc, char** argv) {
    bool synthetic   = false;
    bool autoconnect = false;
    bool verbose     = false;
    int  max_frames  = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--synthetic") == 0) {
            synthetic = true;
        } else if (std::strcmp(argv[i], "--autoconnect") == 0) {
            autoconnect = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        }
    }

    // BrainFlow logs to stderr at info level by default, which floods the
    // console during normal operation. Connection failures already surface in
    // the status bar; --verbose brings the raw log back for diagnosis.
    if (verbose) {
        BoardShim::enable_dev_board_logger();
    } else {
        BoardShim::disable_board_logger();
    }

    UiShell shell("ELANORA - Muse Monitor", 1400, 900);
    if (!shell.ok()) {
        std::fprintf(stderr, "UiShell failed: %s\n", shell.error().c_str());
        return 1;
    }

    MuseDevice device(synthetic ? static_cast<int>(BoardIds::SYNTHETIC_BOARD)
                                : static_cast<int>(BoardIds::MUSE_2_BOARD));
    StreamRecorder recorder;

    // The poll thread must keep draining regardless of what the UI is doing.
    // If it stalls, BrainFlow's internal buffer overruns and samples are lost
    // silently rather than with an error.
    std::atomic<bool> poll_running{false};
    std::thread poll_thread;

    auto start_polling = [&] {
        poll_running = true;
        poll_thread  = std::thread([&] {
            while (poll_running) {
                recorder.poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    };
    auto stop_polling = [&] {
        poll_running = false;
        if (poll_thread.joinable()) poll_thread.join();
    };

    std::array<char, 64> device_id{};
    std::string status = "not connected";
    bool status_is_error = false;

    // Shared by the Connect button and --autoconnect, so the automated check
    // exercises the same path a person does rather than a parallel one.
    auto do_connect = [&] {
        std::string err;
        if (device.connect(device_id.data(), err)) {
            recorder.clear();
            recorder.start(device);
            start_polling();
            status          = err.empty() ? "connected" : ("connected - " + err);
            status_is_error = !err.empty();
        } else {
            status          = err.empty() ? "connect failed" : err;
            status_is_error = true;
        }
    };

    if (autoconnect) do_connect();

    int frames = 0;
    while (shell.begin_frame()) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("monitor", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- connection bar ------------------------------------------------
        ImGui::TextUnformatted(synthetic ? "Synthetic board" : "Muse 2");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##id", "serial or MAC (blank = first found)",
                                 device_id.data(), device_id.size());
        ImGui::SameLine();

        if (!device.connected()) {
            if (ImGui::Button("Connect")) do_connect();
        } else if (ImGui::Button("Disconnect")) {
            stop_polling();
            recorder.stop();
            device.disconnect();
            status          = "not connected";
            status_is_error = false;
        }

        ImGui::SameLine();
        ImGui::TextColored(status_is_error  ? to_imvec(theme::kBad)
                           : device.connected() ? to_imvec(theme::kGood)
                                                : to_imvec(theme::kMuted),
                           "%s", status.c_str());
        ImGui::Separator();

        const ChannelMap& ch = device.channels();
        const double t_eeg = recorder.latest_ts(BrainFlowPresets::DEFAULT_PRESET);
        const double t_imu = recorder.latest_ts(BrainFlowPresets::AUXILIARY_PRESET);
        const double t_ppg = recorder.latest_ts(BrainFlowPresets::ANCILLARY_PRESET);

        const auto eeg_win = recorder.window(BrainFlowPresets::DEFAULT_PRESET,
                                             t_eeg - kPlotSeconds, t_eeg);
        const auto imu_win = recorder.window(BrainFlowPresets::AUXILIARY_PRESET,
                                             t_imu - kPlotSeconds, t_imu);
        const auto ppg_win = recorder.window(BrainFlowPresets::ANCILLARY_PRESET,
                                             t_ppg - kPlotSeconds, t_ppg);

        // ---- electrode quality ----------------------------------------------
        ImGui::TextUnformatted("Electrodes");
        if (ImGui::BeginTable("quality", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("sensor");
            ImGui::TableSetupColumn("electrode");
            ImGui::TableSetupColumn("quality");
            ImGui::TableSetupColumn("RMS uV");
            ImGui::TableSetupColumn("note");
            ImGui::TableHeadersRow();

            for (int i = 0; i < kSensorCount; ++i) {
                const auto sensor = static_cast<SensorId>(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(sensor_name(sensor));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(electrode_name(sensor));

                ChannelQuality q;
                if (i < static_cast<int>(ch.eeg.size())) {
                    const Trace tr =
                        extract(eeg_win, ch.eeg[static_cast<std::size_t>(i)], t_eeg);
                    q = assess(tr.v, ch.sr_eeg);
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(quality_color(q.q), "%s", quality_name(q.q));
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", q.rms_uv);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", q.flat ? "no contact"
                                          : q.railed ? "saturated"
                                                     : "");
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();

        // ---- plots -----------------------------------------------------------
        static const char* kEegLabels[] = {"TP9", "AF7", "AF8", "TP10"};
        static const theme::Rgba kEegColors[] = {theme::kTheta, theme::kAlpha,
                                                 theme::kBeta, theme::kGamma};
        plot_traces("EEG (uV)", eeg_win, t_eeg, ch.eeg, kEegLabels, kEegColors, 4, 240.0f);

        static const char* kPpgLabels[] = {"red 660nm", "IR 940nm", "ambient"};
        plot_traces("PPG", ppg_win, t_ppg, ch.ppg, kPpgLabels, nullptr, 3, 160.0f);

        std::vector<int> imu_rows = ch.accel;
        imu_rows.insert(imu_rows.end(), ch.gyro.begin(), ch.gyro.end());
        static const char* kImuLabels[] = {"ax", "ay", "az", "gx", "gy", "gz"};
        plot_traces("IMU", imu_win, t_imu, imu_rows, kImuLabels, nullptr, 6, 160.0f);

        ImGui::Separator();
        ImGui::TextDisabled("EEG %d Hz / %zu buffered     IMU %d Hz / %zu     PPG %d Hz / %zu",
                            ch.sr_eeg, recorder.size(BrainFlowPresets::DEFAULT_PRESET),
                            ch.sr_imu, recorder.size(BrainFlowPresets::AUXILIARY_PRESET),
                            ch.sr_ppg, recorder.size(BrainFlowPresets::ANCILLARY_PRESET));

        ImGui::End();
        shell.end_frame();

        if (max_frames >= 0 && ++frames >= max_frames) break;
    }

    stop_polling();
    recorder.stop();
    device.disconnect();
    std::printf("muse_monitor: rendered %d frames, shut down cleanly\n", frames);
    return 0;
}
