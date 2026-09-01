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
//   muse_monitor --screenshot P  save the final frame to PNG at P

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
struct SpectrumResult {
    std::array<double, kBandCount> bands{};
    std::vector<double> psd;      // linear magnitude, index * bin_hz = frequency
    double bin_hz = 0.0;
    double peak_hz = 0.0;
};

// One transform serves both the band bars and the spectrum plot, so the two
// panels can never disagree about what the signal contains.
SpectrumResult analyse(std::vector<double> samples, int sr) {
    SpectrumResult out;
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
        out.bin_hz = static_cast<double>(sr) / kNfft;
        out.psd.assign(g.psd.first, g.psd.first + len);

        const double total =
            DataFilter::get_band_power(g.psd, len, kAnalysisLowHz, kAnalysisHighHz);
        if (total <= 0.0) return out;
        for (int b = 0; b < kBandCount; ++b) {
            out.bands[static_cast<std::size_t>(b)] =
                DataFilter::get_band_power(g.psd, len, kBandEdges[b][0], kBandEdges[b][1]) / total;
        }
    } catch (const BrainFlowException&) {
        // A window too short or malformed is not worth interrupting the UI for.
    }
    return out;
}

// Collapses three axes into a single magnitude series, centred on its own mean.
//
// Six raw axes drawn as six lanes conveyed nothing: they are near-identical
// noise, and stacked they read as a solid blob. What actually matters for this
// project is head motion as a scalar -- it is the artifact term that decides
// whether a gamma reading is brain or muscle.
std::vector<double> axis_magnitude(const std::vector<Sample>& win,
                                   const std::vector<int>& rows) {
    std::vector<double> out;
    if (rows.size() < 3 || win.empty()) return out;
    out.reserve(win.size());
    double sum = 0.0;
    int n = 0;
    for (const auto& s : win) {
        double acc = 0.0;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const int row = rows[static_cast<std::size_t>(k)];
            if (row < 0 || row >= static_cast<int>(s.values.size())) { ok = false; break; }
            const double v = s.values[static_cast<std::size_t>(row)];
            if (std::isnan(v)) { ok = false; break; }
            acc += v * v;
        }
        if (!ok) { out.push_back(std::nan("")); continue; }
        const double m = std::sqrt(acc);
        out.push_back(m);
        sum += m; ++n;
    }
    if (n > 0) {
        const double mean = sum / n;
        for (double& v : out) if (!std::isnan(v)) v -= mean;
    }
    return out;
}

void plot_lanes(const char* id, const std::vector<Sample>& win,
                const std::vector<int>& rows, const char* const* labels,
                const theme::Rgba* colors, int n,
                const DisplaySettings& disp, int sr, float height,
                bool apply_filters, double units_full_scale,
                int* dropped_out, float gutter = 0.0f) {
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
        dl->AddLine(ImVec2(origin.x + gutter, std::round(mid) + 0.5f),
                    ImVec2(origin.x + width, std::round(mid) + 0.5f),
                    ImGui::GetColorU32(v4(theme::kLine)), 1.0f);
        if (i > 0) {
            dl->AddLine(ImVec2(origin.x + gutter, std::round(top) + 0.5f),
                        ImVec2(origin.x + width, std::round(top) + 0.5f),
                        ImGui::GetColorU32(ImVec4(theme::kLine.r, theme::kLine.g,
                                                  theme::kLine.b, 0.45f)), 1.0f);
        }

        std::vector<double> sig = channel_of(win, rows[static_cast<std::size_t>(i)]);
        if (sig.empty()) continue;
        if (apply_filters) sig = display_chain(sig, sr, disp);

        dropped += draw_trace_minmax(sig, ImVec2(origin.x + gutter, top),
                                     ImVec2(width - gutter, lane_h),
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
    const char* shot_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--synthetic") == 0)        synthetic = true;
        else if (std::strcmp(argv[i], "--autoconnect") == 0) autoconnect = true;
        else if (std::strcmp(argv[i], "--verbose") == 0)     verbose = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            shot_path = argv[++i];
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

    double bands_at = 0.0;
    int dropped_samples = 0;
    std::array<double, kBandCount> spec_hero{};
    std::array<SpectrumResult, kSensorCount> spec{};
    int focus_sensor = 1;                                 // AF7 by default
    int sens_index = 1;                                   // 50 uV/div

    // Every displayed quantity eases toward its measurement. A band value that
    // jumps between frames forces the eye to re-read it; one that eases lets
    // you see it rising without watching continuously.
    std::array<std::array<Smoothed, kBandCount>, kSensorCount> sm_bands{};
    std::array<Smoothed, kSensorCount> sm_rms{};
    Smoothed sm_usable;
    sm_usable.snap(0.0);
    Smoothed sm_motion;

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

        // Keyboard paths for everything reachable by mouse. An instrument you
        // are wearing on your head is easier to drive without hunting a cursor.
        const ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) sens_index = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_2)) sens_index = 1;
            if (ImGui::IsKeyPressed(ImGuiKey_3)) sens_index = 2;
            if (ImGui::IsKeyPressed(ImGuiKey_4)) sens_index = 3;
            if (ImGui::IsKeyPressed(ImGuiKey_H)) display.highpass = !display.highpass;
            if (ImGui::IsKeyPressed(ImGuiKey_N)) display.notch = !display.notch;
            if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                if (device.connected()) {
                    stop_polling(); recorder.stop(); device.disconnect();
                    status = "Not connected"; status_error = false;
                } else {
                    do_connect();
                }
            }
        }
        const float dt = io.DeltaTime;

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
            right_align(660.0f);

            // Dominant band on the focused sensor, at display size. This is
            // the single number an operator watches while adjusting a headset,
            // and it was previously buried as a 0.89 in a grid cell.
            {
                const auto& fb = spec_hero;
                int dom = 0;
                for (int b = 1; b < kBandCount; ++b) {
                    if (fb[static_cast<std::size_t>(b)] > fb[static_cast<std::size_t>(dom)]) dom = b;
                }
                ImGui::BeginGroup();
                const float y0 = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(y0 - 6.0f);
                if (fonts().display) ImGui::PushFont(fonts().display);
                ImGui::TextColored(v4(theme::band_color(dom)), "%s",
                                   band_name(static_cast<Band>(dom)));
                if (fonts().display) ImGui::PopFont();
                ImGui::EndGroup();
                ImGui::SameLine(0.0f, theme::kS3);
                ImGui::BeginGroup();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
                text_colored(theme::kDim, "%.2f", fb[static_cast<std::size_t>(dom)]);
                text_colored(theme::kFaint, "dominant");
                ImGui::EndGroup();
            }

            ImGui::SameLine(0.0f, theme::kS5);
            text_mono(theme::kFaint, "space  1-4  H  N");
            ImGui::SameLine(0.0f, theme::kS4);
            text_mono(theme::kMuted, "EEG %d Hz   IMU %d Hz   PPG %d Hz",
                      ch.sr_eeg, ch.sr_imu, ch.sr_ppg);
            end_card();
        } else { end_card(); }

        // ---- EEG: the primary panel, given the space it deserves ----------
        const float gap = theme::kS3;
        const float lower_h = 126.0f;
        const float mid_h   = 292.0f;
        const float eeg_h = std::max(200.0f,
            ImGui::GetContentRegionAvail().y - lower_h - mid_h - gap * 2.0f);

        // Recomputed once per interval and shared by every panel below, so the
        // spectrum, the matrix and the bars all describe the same window.
        const double now_t = ImGui::GetTime();
        if (now_t - bands_at > 0.15) {
            bands_at = now_t;
            for (int i = 0; i < kSensorCount; ++i) {
                if (i < static_cast<int>(ch.eeg.size())) {
                    spec[static_cast<std::size_t>(i)] =
                        analyse(channel_of(eeg_win, ch.eeg[static_cast<std::size_t>(i)]),
                                ch.sr_eeg);
                }
            }
        }
        spec_hero = spec[static_cast<std::size_t>(focus_sensor)].bands;

        if (begin_card("##eegcard", ImVec2(0, eeg_h))) {
            eyebrow("EEG");
            ImGui::SameLine();
            right_align(600.0f);

            text_mono(theme::kFaint, "uV/div");
            ImGui::SameLine(0.0f, theme::kS2);
            static const char* kSensLabels[] = {"25", "50", "100", "200"};
            static const double kSensValues[] = {25.0, 50.0, 100.0, 200.0};
            segmented("##sens", kSensLabels, 4, &sens_index, 176.0f);
            display.uv_per_div = kSensValues[sens_index];

            ImGui::SameLine(0.0f, theme::kS4);
            toggle_switch("##hp", &display.highpass);
            ImGui::SameLine(0.0f, theme::kS2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
            text_colored(display.highpass ? theme::kDim : theme::kFaint, "High-pass");

            ImGui::SameLine(0.0f, theme::kS4);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f);
            toggle_switch("##notch", &display.notch);
            ImGui::SameLine(0.0f, theme::kS2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
            text_colored(display.notch ? theme::kDim : theme::kFaint, "Notch 60");

            static const char* kEegLabels[] = {"TP9", "AF7", "AF8", "TP10"};
            static const theme::Rgba kEegColors[] = {
                theme::kTrace, theme::kTrace, theme::kTrace, theme::kTrace};

            constexpr float kGutter = 52.0f;   // label + calibration column
            const float axis_h = ImGui::GetTextLineHeight() + 4.0f;

            const ImVec2 plot_origin = ImGui::GetCursorScreenPos();
            const float plot_w = ImGui::GetContentRegionAvail().x;
            const float caption_h = ImGui::GetTextLineHeight() + 2.0f;
            const float plot_h = ImGui::GetContentRegionAvail().y - theme::kS2 - axis_h - caption_h;

            // Grid first so the traces sit on top of it, and only across the
            // trace area -- gridlines through the label gutter read as noise.
            draw_time_grid(ImVec2(plot_origin.x + kGutter, plot_origin.y),
                           ImVec2(plot_w - kGutter, plot_h), kWindowSeconds, 1.0);

            plot_lanes("eeg", eeg_win, ch.eeg, kEegLabels, kEegColors, kSensorCount,
                       display, ch.sr_eeg, plot_h,
                       true, display.uv_per_div * 2.0, &dropped_samples, kGutter);

            // Calibration mark lives in the gutter, clear of every trace.
            draw_scale_bar(ImVec2(plot_origin.x + 30.0f, plot_origin.y),
                           plot_h / kSensorCount, display.uv_per_div, theme::kMuted);

            ImGui::Dummy(ImVec2(1.0f, axis_h));

            text_mono(theme::kFaint,
                      "min/max decimation   %.0f uV/div   %s   %s   %.0f s   %s",
                      display.uv_per_div,
                      display.highpass ? "HP 0.5 Hz" : "HP off",
                      display.notch ? "notch 60 Hz" : "notch off",
                      kWindowSeconds,
                      dropped_samples > 0 ? "DROPPED SAMPLES" : "no dropouts");
        }
        end_card();

        // ---- spectrum | band matrix | electrodes ---------------------------
        const float avail  = ImGui::GetContentRegionAvail().x;
        const float w_spec = (avail - gap * 2.0f) * 0.30f;
        const float w_mtx  = (avail - gap * 2.0f) * 0.42f;
        const float w_el   = (avail - gap * 2.0f) * 0.28f;

        if (begin_card("##speccard", ImVec2(w_spec, mid_h))) {
            eyebrow("Spectrum");
            ImGui::SameLine();
            right_align(160.0f);
            text_mono(theme::kFaint, "%s   peak %.0f Hz",
                      electrode_name(static_cast<SensorId>(focus_sensor)),
                      spec[static_cast<std::size_t>(focus_sensor)].peak_hz);

            const ImVec2 o = ImGui::GetCursorScreenPos();
            const ImVec2 sz(ImGui::GetContentRegionAvail().x,
                            ImGui::GetContentRegionAvail().y - 4.0f);
            auto& sp = spec[static_cast<std::size_t>(focus_sensor)];
            if (!sp.psd.empty()) {
                double peak = 0.0;
                draw_spectrum(sp.psd.data(), static_cast<int>(sp.psd.size()), sp.bin_hz,
                              o, sz, kAnalysisHighHz, &peak);
                sp.peak_hz = peak;
            }
            ImGui::Dummy(sz);
        }
        end_card();

        ImGui::SameLine(0.0f, gap);
        if (begin_card("##mtxcard", ImVec2(w_mtx, mid_h))) {
            eyebrow("Relative band power");
            ImGui::SameLine();
            right_align(190.0f);
            text_mono(theme::kFaint, "click a row to focus");

            // This 4x5 grid IS the measurement: four brain models, five bands
            // each. Showing one sensor at a time hides three quarters of it.
            const float label_w = 48.0f;
            const float grid_w  = ImGui::GetContentRegionAvail().x - label_w;
            const float cell_w  = (grid_w - theme::kS1 * (kBandCount - 1)) / kBandCount;
            const float head_h  = ImGui::GetTextLineHeight() + 3.0f;

            const ImVec2 head_origin = ImGui::GetCursorScreenPos();
            ImDrawList* hdl = ImGui::GetWindowDrawList();
            for (int b = 0; b < kBandCount; ++b) {
                const char* nm = band_name(static_cast<Band>(b));
                const float bx = head_origin.x + label_w + (cell_w + theme::kS1) * b;
                // A small dot carries the band hue; the label itself stays
                // neutral, so the header is legible rather than a rainbow.
                hdl->AddCircleFilled(ImVec2(bx + 3.0f, head_origin.y + 7.0f), 3.0f,
                                     ImGui::GetColorU32(v4(theme::band_color(b))));
                hdl->AddText(ImVec2(bx + 11.0f, head_origin.y),
                             ImGui::GetColorU32(v4(theme::kMuted)), nm);
            }
            ImGui::Dummy(ImVec2(1.0f, head_h));

            // Measured AFTER the header, because SetCursorScreenPos below
            // bypasses ImGui item spacing: budgeting before the header left
            // one row worth of spacing unaccounted for and clipped TP10.
            // Six px of tail so the last row's bar does not sit on the card
            // edge, which reads as clipped even when it is fully drawn.
            const float body_h = ImGui::GetContentRegionAvail().y - 6.0f;
            const float cell_h =
                (body_h - theme::kS1 * kSensorCount) / kSensorCount;

            for (int r = 0; r < kSensorCount; ++r) {
                const auto& row = spec[static_cast<std::size_t>(r)].bands;
                int dom = 0;
                for (int b = 1; b < kBandCount; ++b) {
                    if (row[static_cast<std::size_t>(b)] >
                        row[static_cast<std::size_t>(dom)]) dom = b;
                }
                for (int b = 0; b < kBandCount; ++b) {
                    sm_bands[static_cast<std::size_t>(r)][static_cast<std::size_t>(b)]
                        .set(row[static_cast<std::size_t>(b)], dt);
                }

                const ImVec2 ro = ImGui::GetCursorScreenPos();
                const float row_w = ImGui::GetContentRegionAvail().x;

                // Whole row is the hit target, and it highlights on hover, so
                // "click a row to focus" is discoverable rather than a caption
                // asking you to guess where.
                char rid[16];
                std::snprintf(rid, sizeof(rid), "##row%d", r);
                ImGui::SetCursorScreenPos(ro);
                ImGui::InvisibleButton(rid, ImVec2(row_w, cell_h));
                if (ImGui::IsItemClicked()) focus_sensor = r;
                const bool hot = ImGui::IsItemHovered() || r == focus_sensor;

                ImDrawList* rdl = ImGui::GetWindowDrawList();
                if (hot) {
                    rdl->AddRectFilled(ImVec2(ro.x - theme::kS2, ro.y),
                                       ImVec2(ro.x + row_w, ro.y + cell_h),
                                       ImGui::GetColorU32(v4(theme::kRaised)),
                                       theme::kRadiusSm);
                }

                // Label and values share one baseline. Previously the label
                // sat at the row top while band_cell centred its value, so a
                // row read as belonging to the row above it.
                const float text_h = ImGui::GetTextLineHeight();
                const float text_y = ro.y + (cell_h - 3.0f - text_h) * 0.5f;
                rdl->AddText(ImVec2(ro.x, text_y),
                             ImGui::GetColorU32(v4(r == focus_sensor ? theme::kText
                                                                     : theme::kMuted)),
                             electrode_name(static_cast<SensorId>(r)));

                for (int b = 0; b < kBandCount; ++b) {
                    band_cell(ImVec2(ro.x + label_w + (cell_w + theme::kS1) * b, ro.y),
                              ImVec2(cell_w, cell_h),
                              sm_bands[static_cast<std::size_t>(r)]
                                      [static_cast<std::size_t>(b)].value,
                              theme::band_color(b), b == dom);
                }
                ImGui::SetCursorScreenPos(ImVec2(ro.x, ro.y + cell_h + theme::kS1));
            }
        }
        end_card();

        ImGui::SameLine(0.0f, gap);
        if (begin_card("##elcard", ImVec2(w_el, mid_h))) {
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
                sm_rms[static_cast<std::size_t>(i)].set(q[static_cast<std::size_t>(i)].rms_uv, dt);
            }
            sm_usable.set(static_cast<double>(usable), dt);
            char cnt[8];
            std::snprintf(cnt, sizeof(cnt), "%.0f", sm_usable.value);
            readout(cnt, "OF 4 USABLE",
                    usable == kSensorCount ? theme::kText : theme::kWarn);
            ImGui::Spacing();

            for (int i = 0; i < kSensorCount; ++i) {
                const auto& cq = q[static_cast<std::size_t>(i)];
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                const float h = 26.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                                  ImGui::GetColorU32(v4(theme::kRaised)), theme::kRadiusSm);
                dl->AddRectFilled(p0, ImVec2(p0.x + 2.0f, p0.y + h),
                                  ImGui::GetColorU32(v4(quality_color(cq.q))));
                ImGui::SetCursorScreenPos(ImVec2(p0.x + theme::kS3, p0.y + 4.0f));
                ImGui::TextColored(v4(theme::kDim), "%s",
                                   electrode_name(static_cast<SensorId>(i)));
                ImGui::SameLine(0.0f, theme::kS4);
                ImGui::TextColored(v4(theme::kMuted), "%.0f uV",
                                   sm_rms[static_cast<std::size_t>(i)].value);
                ImGui::SameLine();
                right_align(78.0f);
                ImGui::TextColored(v4(quality_color(cq.q)), "%s",
                                   cq.flat ? "no contact"
                                   : cq.railed ? "saturated"
                                   : quality_name(cq.q));
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + theme::kS1));
            }
        }
        end_card();

        // ---- PPG / IMU ------------------------------------------------------
        const bool has_ppg = !ch.ppg.empty();
        const float col2 = has_ppg ? (ImGui::GetContentRegionAvail().x - gap) / 2.0f
                                   : 0.0f;
        if (has_ppg && begin_card("##ppgcard", ImVec2(col2, lower_h))) {
            eyebrow("PPG");
            ImGui::SameLine();
            right_align(190.0f);
            text_mono(theme::kFaint, "[0] red 660  [1] IR 940");
            static const char* kPpgLabels[] = {"red", "IR", "amb"};
            static const theme::Rgba kPpgColors[] = {
                theme::kTrace, theme::kTraceAlt, theme::kFaint};
            // PPG is raw counts, not microvolts, and its DC level is large --
            // the high-pass is what makes the pulse visible at all here.
            DisplaySettings ppg_disp; ppg_disp.highpass = true; ppg_disp.notch = false;
            plot_lanes("ppg", ppg_win, ch.ppg, kPpgLabels, kPpgColors,
                       static_cast<int>(std::min<std::size_t>(ch.ppg.size(), 3)),
                       ppg_disp, ch.sr_ppg,
                       ImGui::GetContentRegionAvail().y - 4.0f, true, 4000.0, nullptr);
        }
        if (has_ppg) { end_card(); ImGui::SameLine(0.0f, gap); }

        if (begin_card("##imucard", ImVec2(has_ppg ? col2 : 0.0f, lower_h))) {
            eyebrow("Head motion");

            const auto accel_mag = axis_magnitude(imu_win, ch.accel);
            const auto gyro_mag  = axis_magnitude(imu_win, ch.gyro);

            // Motion energy over the last second: the artifact term that
            // decides whether a high-frequency EEG reading is brain or muscle.
            double energy = 0.0;
            int counted = 0;
            const std::size_t tail = accel_mag.size() > 52 ? accel_mag.size() - 52 : 0;
            for (std::size_t i = tail; i < accel_mag.size(); ++i) {
                if (!std::isnan(accel_mag[i])) { energy += accel_mag[i] * accel_mag[i]; ++counted; }
            }
            energy = counted ? std::sqrt(energy / counted) : 0.0;
            sm_motion.set(energy, dt);

            ImGui::SameLine();
            right_align(190.0f);
            const bool still = sm_motion.value < 0.05;
            text_colored(still ? theme::kGood : theme::kWarn,
                         still ? "still" : "moving");
            ImGui::SameLine(0.0f, theme::kS3);
            text_mono(theme::kMuted, "%.3f rms", sm_motion.value);

            const ImVec2 o = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            if (accel_mag.empty()) {
                ImDrawList* d0 = ImGui::GetWindowDrawList();
                const char* msg = "waiting for samples";
                const ImVec2 ts = ImGui::CalcTextSize(msg);
                d0->AddText(ImVec2(o.x + (w - ts.x) * 0.5f, o.y + (h - ts.y) * 0.5f),
                            ImGui::GetColorU32(v4(theme::kFaint)), msg);
            } else {
                // No fill on these: they overlap, and filled overlapping traces
                // merge into a single mass that shows nothing.
                draw_trace_minmax(accel_mag, o, ImVec2(w, h), 0.35,
                                  theme::kTrace, false);
                draw_trace_minmax(gyro_mag, o, ImVec2(w, h), 40.0,
                                  theme::kTraceAlt, false);
            }
            ImGui::Dummy(ImVec2(w, h));
        }
        end_card();

        ImGui::End();
        ImGui::PopStyleVar(2);
        shell.end_frame();

        ++frames;
        // Capture the last frame, once the smoothed values have settled.
        if (shot_path != nullptr && max_frames > 0 && frames == max_frames) {
            if (shell.save_screenshot(shot_path)) {
                std::printf("muse_monitor: wrote %s\n", shot_path);
            }
        }
        if (max_frames >= 0 && frames >= max_frames) break;
    }

    stop_polling();
    recorder.stop();
    device.disconnect();
    std::printf("muse_monitor: rendered %d frames, shut down cleanly\n", frames);
    return 0;
}
