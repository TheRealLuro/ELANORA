// muse_monitor -- live Muse 2 signal viewer.
//
// The gate for everything downstream: if the four EEG traces are not moving,
// the PPG pulse is not visible, and the IMU does not respond to head motion,
// then no dataset collected afterwards means anything.
//
//   muse_monitor                 connect to the first Muse 2 found
//   muse_monitor --synthetic     run against BrainFlow's synthetic board
//   muse_monitor --autoconnect   connect on launch
//   muse_monitor --frames N      render N frames then exit (automation)
//   muse_monitor --verbose       leave BrainFlow's stderr logging on

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "elanora/lsl/muse_device.hpp"
#include "elanora/lsl/signal_quality.hpp"
#include "elanora/lsl/stream_recorder.hpp"
#include "elanora/render.hpp"
#include "elanora/theme.hpp"
#include "elanora/types.hpp"
#include "elanora/ui_shell.hpp"

#include "data_filter.h"
#include "imgui.h"
#include "implot.h"

using namespace elanora;
using namespace elanora::lsl;

namespace {

constexpr double kWindowSeconds = 8.0;

ImVec4 v4(theme::Rgba c) { return ImVec4{c.r, c.g, c.b, c.a}; }

theme::Rgba quality_color(Quality q) {
    switch (q) {
        case Quality::Good: return theme::kGood;
        case Quality::Fair: return theme::kWarn;
        case Quality::Bad:  return theme::kBad;
    }
    return theme::kMuted;
}

// Pulls one channel of a window out in acquisition order.
std::vector<double> channel_of(const std::vector<Sample>& win, int row) {
    std::vector<double> out;
    if (row < 0) return out;
    out.reserve(win.size());
    for (const auto& s : win) {
        out.push_back(row < static_cast<int>(s.values.size())
                          ? s.values[static_cast<std::size_t>(row)]
                          : std::nan(""));
    }
    return out;
}

// BrainFlow hands back raw arrays from get_psd_welch; this frees them even if
// something between allocation and use throws.
struct PsdGuard {
    std::pair<double*, double*> psd{nullptr, nullptr};
    ~PsdGuard() { delete[] psd.first; delete[] psd.second; }
};

// Relative band power for one channel, using the same BrainFlow calls the
// feature pipeline will use, so the monitor and the dataset cannot disagree.
std::array<double, kBandCount> relative_bands(std::vector<double> samples, int sr) {
    std::array<double, kBandCount> out{};
    constexpr int kNfft = 256;
    if (static_cast<int>(samples.size()) < kNfft || sr <= 0) return out;

    // A dropped packet must not poison the transform; zero it for the FFT only.
    for (double& v : samples) if (std::isnan(v)) v = 0.0;

    try {
        DataFilter::detrend(samples.data(), static_cast<int>(samples.size()),
                            static_cast<int>(DetrendOperations::LINEAR));
        PsdGuard g;
        // Signature verified against data_filter.h, not the online docs, which
        // list a 5-argument form that does not exist: psd_len is an out-param.
        int len = 0;
        g.psd = DataFilter::get_psd_welch(samples.data(), static_cast<int>(samples.size()),
                                          kNfft, kNfft / 2, sr,
                                          static_cast<int>(WindowOperations::HANNING), &len);
        if (len <= 0) return out;
        double total = DataFilter::get_band_power(g.psd, len, kAnalysisLowHz, kAnalysisHighHz);
        if (total <= 0.0) return out;
        for (int b = 0; b < kBandCount; ++b) {
            out[static_cast<std::size_t>(b)] =
                DataFilter::get_band_power(g.psd, len, kBandEdges[b][0], kBandEdges[b][1]) / total;
        }
    } catch (const BrainFlowException&) {
        // A window too short or malformed is not worth interrupting the UI for.
    }
    return out;
}

void plot_lanes(const char* id, const std::vector<Sample>& win,
                const std::vector<int>& rows, const char* const* labels,
                const theme::Rgba* colors, int n,
                const DisplaySettings& disp, int sr, float height,
                bool apply_filters, double units_full_scale,
                int* dropped_out) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    if (width < 4.0f) return;

    // An empty panel reads as a broken one. Say why there is nothing to draw:
    // the synthetic board has no PPG preset, and a real headset may not have
    // finished negotiating the stream yet.
    if (n <= 0 || win.empty()) {
        ImDrawList* dl0 = ImGui::GetWindowDrawList();
        const char* msg = (n <= 0) ? "stream not available on this board"
                                   : "waiting for samples";
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl0->AddText(ImVec2(origin.x + (width - ts.x) * 0.5f,
                            origin.y + (height - ts.y) * 0.5f),
                     ImGui::GetColorU32(v4(theme::kFaint)), msg);
        ImGui::Dummy(ImVec2(width, height));
        return;
    }

    const float lane_h = height / static_cast<float>(n);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int dropped = 0;

    for (int i = 0; i < n && i < static_cast<int>(rows.size()); ++i) {
        const float top = origin.y + lane_h * static_cast<float>(i);
        const float mid = top + lane_h * 0.5f;

        // Centre line for this lane, and a fainter separator between lanes.
        dl->AddLine(ImVec2(origin.x, std::round(mid) + 0.5f),
                    ImVec2(origin.x + width, std::round(mid) + 0.5f),
                    ImGui::GetColorU32(v4(theme::kLine)), 1.0f);
        if (i > 0) {
            dl->AddLine(ImVec2(origin.x, std::round(top) + 0.5f),
                        ImVec2(origin.x + width, std::round(top) + 0.5f),
                        ImGui::GetColorU32(ImVec4(theme::kLine.r, theme::kLine.g,
                                                  theme::kLine.b, 0.45f)), 1.0f);
        }

        std::vector<double> sig = channel_of(win, rows[static_cast<std::size_t>(i)]);
        if (sig.empty()) continue;
        if (apply_filters) sig = display_chain(sig, sr, disp);

        dropped += draw_trace_minmax(sig, ImVec2(origin.x, top), ImVec2(width, lane_h),
                                     units_full_scale,
                                     colors ? colors[i] : theme::kAccent, true);

        // Channel label sits inside its own lane, left-aligned.
        if (labels && labels[i]) {
            if (fonts().mono) ImGui::PushFont(fonts().mono);
            dl->AddText(ImVec2(origin.x + 4.0f, top + 3.0f),
                        ImGui::GetColorU32(v4(theme::kFaint)), labels[i]);
            if (fonts().mono) ImGui::PopFont();
        }
    }
    if (dropped_out) *dropped_out = dropped;
    ImGui::Dummy(ImVec2(width, height));
    (void)id;
}

}  // namespace

int main(int argc, char** argv) {
    bool synthetic = false, autoconnect = false, verbose = false;
    int max_frames = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--synthetic") == 0)        synthetic = true;
        else if (std::strcmp(argv[i], "--autoconnect") == 0) autoconnect = true;
        else if (std::strcmp(argv[i], "--verbose") == 0)     verbose = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = std::atoi(argv[++i]);
    }

    // BrainFlow logs to stderr at info level by default, which floods the
    // console. Connection failures already surface in the status bar.
    if (verbose) BoardShim::enable_dev_board_logger();
    else         BoardShim::disable_board_logger();

    UiShell shell("ELANORA - Muse Monitor", 1480, 940);
    if (!shell.ok()) {
        std::fprintf(stderr, "UiShell failed: %s\n", shell.error().c_str());
        return 1;
    }
    shell.set_clear_color(theme::kGround.r, theme::kGround.g, theme::kGround.b);

    MuseDevice device(synthetic ? static_cast<int>(BoardIds::SYNTHETIC_BOARD)
                                : static_cast<int>(BoardIds::MUSE_2_BOARD));
    StreamRecorder recorder;
    DisplaySettings display;

    // The poll thread must keep draining regardless of what the UI is doing.
    // If it stalls, BrainFlow's internal buffer overruns and samples are lost
    // silently rather than with an error.
    std::atomic<bool> poll_running{false};
    std::thread poll_thread;
    auto start_polling = [&] {
        poll_running = true;
        poll_thread = std::thread([&] {
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
    std::string status = "Not connected";
    bool status_error = false;

    auto do_connect = [&] {
        std::string err;
        if (device.connect(device_id.data(), err)) {
            recorder.clear();
            recorder.start(device);
            start_polling();
            status = err.empty() ? "Streaming" : ("Streaming - " + err);
            status_error = !err.empty();
        } else {
            status = err.empty() ? "Connect failed" : err;
            status_error = true;
        }
    };
    if (autoconnect) do_connect();

    std::array<double, kBandCount> bands{};
    double bands_at = 0.0;
    int dropped_samples = 0;

    int frames = 0;
    while (shell.begin_frame()) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

        const ChannelMap& ch = device.channels();
        const double t_eeg = recorder.latest_ts(BrainFlowPresets::DEFAULT_PRESET);
        const double t_imu = recorder.latest_ts(BrainFlowPresets::AUXILIARY_PRESET);
        const double t_ppg = recorder.latest_ts(BrainFlowPresets::ANCILLARY_PRESET);
        const auto eeg_win = recorder.window(BrainFlowPresets::DEFAULT_PRESET,
                                             t_eeg - kWindowSeconds, t_eeg);
        const auto imu_win = recorder.window(BrainFlowPresets::AUXILIARY_PRESET,
                                             t_imu - kWindowSeconds, t_imu);
        const auto ppg_win = recorder.window(BrainFlowPresets::ANCILLARY_PRESET,
                                             t_ppg - kWindowSeconds, t_ppg);

        // ---- top bar ------------------------------------------------------
        if (begin_card("##topbar", ImVec2(0, 58))) {
            if (fonts().subhead) ImGui::PushFont(fonts().subhead);
            ImGui::TextUnformatted("Elanora");
            if (fonts().subhead) ImGui::PopFont();
            ImGui::SameLine(0.0f, theme::kS2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
            eyebrow(synthetic ? "Synthetic" : "Muse 2");

            ImGui::SameLine(0.0f, theme::kS4);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f);
            status_pill(status.c_str(),
                        status_error ? theme::kBad
                                     : device.connected() ? theme::kGood : theme::kMuted);

            ImGui::SameLine(0.0f, theme::kS4);
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##id", "serial or MAC (blank = first found)",
                                     device_id.data(), device_id.size());

            ImGui::SameLine(0.0f, theme::kS2);
            if (!device.connected()) {
                if (ImGui::Button("Connect")) do_connect();
            } else if (ImGui::Button("Disconnect")) {
                stop_polling(); recorder.stop(); device.disconnect();
                status = "Not connected"; status_error = false;
            }

            ImGui::SameLine();
            right_align(300.0f);
            text_mono(theme::kMuted, "EEG %d Hz   IMU %d Hz   PPG %d Hz",
                      ch.sr_eeg, ch.sr_imu, ch.sr_ppg);
            end_card();
        } else { end_card(); }

        // ---- row 1: device / electrodes / band power -----------------------
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = theme::kS3;
        const float col3 = (avail - gap * 2.0f) / 3.0f;
        const float row1_h = 168.0f;

        if (begin_card("##devcard", ImVec2(col3, row1_h))) {
            eyebrow("Buffered");
            ImGui::Spacing();
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f",
                          static_cast<double>(recorder.size(BrainFlowPresets::DEFAULT_PRESET)));
            readout(buf, "EEG SAMPLES");
            ImGui::Spacing();
            text_mono(theme::kMuted, "IMU  %zu", recorder.size(BrainFlowPresets::AUXILIARY_PRESET));
            text_mono(theme::kMuted, "PPG  %zu", recorder.size(BrainFlowPresets::ANCILLARY_PRESET));
            if (dropped_samples > 0) {
                text_mono(theme::kWarn, "%d dropped", dropped_samples);
            } else {
                text_mono(theme::kFaint, "no dropouts");
            }
        }
        end_card();

        ImGui::SameLine(0.0f, gap);
        if (begin_card("##elcard", ImVec2(col3, row1_h))) {
            eyebrow("Electrode integrity");
            ImGui::Spacing();

            std::array<ChannelQuality, kSensorCount> q{};
            int usable = 0;
            for (int i = 0; i < kSensorCount; ++i) {
                if (i < static_cast<int>(ch.eeg.size())) {
                    q[static_cast<std::size_t>(i)] =
                        assess(channel_of(eeg_win, ch.eeg[static_cast<std::size_t>(i)]),
                               ch.sr_eeg);
                }
                if (q[static_cast<std::size_t>(i)].q != Quality::Bad) ++usable;
            }
            char cnt[8]; std::snprintf(cnt, sizeof(cnt), "%d", usable);
            readout(cnt, "OF 4 USABLE",
                    usable == kSensorCount ? theme::kText : theme::kWarn);
            ImGui::Spacing();

            const float chip_w = (ImGui::GetContentRegionAvail().x - theme::kS1 * 3.0f) / 4.0f;
            for (int i = 0; i < kSensorCount; ++i) {
                if (i) ImGui::SameLine(0.0f, theme::kS1);
                const auto& cq = q[static_cast<std::size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_ChildBg, v4(theme::kRaised));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::kRadiusSm);
                char chip_id[16];
                std::snprintf(chip_id, sizeof(chip_id), "##chip%d", i);
                ImGui::BeginChild(chip_id, ImVec2(chip_w, 46.0f),
                                  ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
                // State reaches the chip as a stripe AND a number, so it does
                // not depend on colour vision or on reading fine text.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetWindowPos();
                dl->AddRectFilled(p, ImVec2(p.x + 2.0f, p.y + 46.0f),
                                  ImGui::GetColorU32(v4(quality_color(cq.q))));
                ImGui::SetCursorPos(ImVec2(theme::kS2, 5.0f));
                text_mono(theme::kDim, "%s", electrode_name(static_cast<SensorId>(i)));
                ImGui::SetCursorPosX(theme::kS2);
                text_mono(quality_color(cq.q), "%.0f uV", cq.rms_uv);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
            }
        }
        end_card();

        ImGui::SameLine(0.0f, gap);
        if (begin_card("##bandcard", ImVec2(col3, row1_h))) {
            eyebrow("Band power");
            ImGui::SameLine();
            right_align(90.0f);
            text_mono(theme::kFaint, "AF7 rel.");
            ImGui::Spacing();

            const double now = ImGui::GetTime();
            if (now - bands_at > 0.15 && ch.eeg.size() > 1) {
                bands_at = now;
                bands = relative_bands(channel_of(eeg_win, ch.eeg[1]), ch.sr_eeg);
            }
            for (int b = 0; b < kBandCount; ++b) {
                const auto col = theme::band_color(b);
                if (fonts().mono) ImGui::PushFont(fonts().mono);
                ImGui::TextColored(v4(col), "%-6s", band_name(static_cast<Band>(b)));
                if (fonts().mono) ImGui::PopFont();
                ImGui::SameLine(72.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, v4(col));
                ImGui::PushStyleColor(ImGuiCol_FrameBg, v4(theme::kRaised));
                char ov[16];
                std::snprintf(ov, sizeof(ov), "%.2f", bands[static_cast<std::size_t>(b)]);
                ImGui::ProgressBar(static_cast<float>(bands[static_cast<std::size_t>(b)]),
                                   ImVec2(-1.0f, 12.0f), ov);
                ImGui::PopStyleColor(2);
            }
        }
        end_card();

        // ---- EEG -----------------------------------------------------------
        const float lower_h = 236.0f;
        const float eeg_h = std::max(180.0f,
            ImGui::GetContentRegionAvail().y - lower_h - gap * 2.0f);

        if (begin_card("##eegcard", ImVec2(0, eeg_h))) {
            eyebrow("EEG");
            ImGui::SameLine();
            right_align(560.0f);

            text_mono(theme::kFaint, "uV/div");
            ImGui::SameLine(0.0f, theme::kS2);
            for (double uv : {50.0, 100.0, 200.0}) {
                const bool on = std::abs(display.uv_per_div - uv) < 0.5;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, v4(theme::kAccent));
                char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%.0f", uv);
                if (ImGui::SmallButton(lbl)) display.uv_per_div = uv;
                if (on) ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, theme::kS1);
            }
            ImGui::SameLine(0.0f, theme::kS3);
            ImGui::Checkbox("HP 0.5Hz", &display.highpass);
            ImGui::SameLine(0.0f, theme::kS2);
            ImGui::Checkbox("Notch 60", &display.notch);

            static const char* kEegLabels[] = {"TP9", "AF7", "AF8", "TP10"};
            static const theme::Rgba kEegColors[] = {
                theme::kTheta, theme::kAlpha, theme::kBeta, theme::kGamma};

            // Full scale is two divisions per lane. Values are BrainFlow
            // microvolts, so this is a real sensitivity, not a fudge factor.
            plot_lanes("eeg", eeg_win, ch.eeg, kEegLabels, kEegColors, kSensorCount,
                       display, ch.sr_eeg,
                       ImGui::GetContentRegionAvail().y - theme::kS2,
                       true, display.uv_per_div * 2.0, &dropped_samples);

            text_mono(theme::kFaint,
                      "min/max decimation  %.0f uV/div  %s  %s  %.0f s window",
                      display.uv_per_div,
                      display.highpass ? "HP 0.5 Hz" : "HP off",
                      display.notch ? "notch 60 Hz" : "notch off",
                      kWindowSeconds);
        }
        end_card();

        // ---- PPG / IMU ------------------------------------------------------
        const float col2 = (ImGui::GetContentRegionAvail().x - gap) / 2.0f;
        if (begin_card("##ppgcard", ImVec2(col2, lower_h))) {
            eyebrow("PPG");
            ImGui::SameLine();
            right_align(190.0f);
            text_mono(theme::kFaint, "[0] red 660  [1] IR 940");
            static const char* kPpgLabels[] = {"red", "IR", "amb"};
            static const theme::Rgba kPpgColors[] = {theme::kGamma, theme::kAlpha, theme::kFaint};
            // PPG is raw counts, not microvolts, and its DC level is large --
            // the high-pass is what makes the pulse visible at all here.
            DisplaySettings ppg_disp; ppg_disp.highpass = true; ppg_disp.notch = false;
            plot_lanes("ppg", ppg_win, ch.ppg, kPpgLabels, kPpgColors,
                       static_cast<int>(std::min<std::size_t>(ch.ppg.size(), 3)),
                       ppg_disp, ch.sr_ppg,
                       ImGui::GetContentRegionAvail().y - 4.0f, true, 4000.0, nullptr);
        }
        end_card();

        ImGui::SameLine(0.0f, gap);
        if (begin_card("##imucard", ImVec2(col2, lower_h))) {
            eyebrow("IMU");
            ImGui::SameLine();
            right_align(150.0f);
            text_mono(theme::kFaint, "accel + gyro, 6 axis");
            std::vector<int> imu_rows = ch.accel;
            imu_rows.insert(imu_rows.end(), ch.gyro.begin(), ch.gyro.end());
            static const char* kImuLabels[] = {"ax","ay","az","gx","gy","gz"};
            static const theme::Rgba kImuColors[] = {
                theme::kTheta, theme::kTheta, theme::kTheta,
                theme::kDelta, theme::kDelta, theme::kDelta};
            DisplaySettings imu_disp; imu_disp.highpass = false; imu_disp.notch = false;
            plot_lanes("imu", imu_win, imu_rows, kImuLabels, kImuColors,
                       static_cast<int>(std::min<std::size_t>(imu_rows.size(), 6)),
                       imu_disp, ch.sr_imu,
                       ImGui::GetContentRegionAvail().y - 4.0f, false, 2.0, nullptr);
        }
        end_card();

        ImGui::End();
        ImGui::PopStyleVar(2);
        shell.end_frame();

        if (max_frames >= 0 && ++frames >= max_frames) break;
    }

    stop_polling();
    recorder.stop();
    device.disconnect();
    std::printf("muse_monitor: rendered %d frames, shut down cleanly\n", frames);
    return 0;
}
