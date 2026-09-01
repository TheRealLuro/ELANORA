// muse_monitor -- numeric Muse 2 signal dashboard.
//
// No waveforms. Every check an operator makes while seating a headset is a
// number: is each electrode in range, is the head still, which band dominates,
// where is the spectral peak. Traces looked busy and answered none of those at
// a glance.
//
// The Milestone 1 hardware gate becomes numeric rather than visual: electrode
// RMS in the 5-50 uV band, motion RMS settling toward zero when still, and
// alpha rising on the frontal sensors when the eyes close. That is a stronger
// check than eyeballing a trace, because it is measured.
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

using namespace elanora;
using namespace elanora::lsl;

namespace {

constexpr double kWindowSeconds = 4.0;

ImVec4 v4(theme::Rgba c) { return ImVec4{c.r, c.g, c.b, c.a}; }

theme::Rgba quality_color(Quality q) {
    switch (q) {
        case Quality::Good: return theme::kGood;
        case Quality::Fair: return theme::kWarn;
        case Quality::Bad:  return theme::kBad;
    }
    return theme::kMuted;
}

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

// BrainFlow returns raw arrays from get_psd_welch; freed even if the code
// between allocation and use throws.
struct PsdGuard {
    std::pair<double*, double*> psd{nullptr, nullptr};
    ~PsdGuard() { delete[] psd.first; delete[] psd.second; }
};

struct Spectrum {
    std::array<double, kBandCount> bands{};
    double peak_hz = 0.0;
    bool   valid   = false;
};

// Uses the same BrainFlow calls the feature pipeline will, so the dashboard
// and the dataset cannot disagree about what a window contains.
Spectrum analyse(std::vector<double> samples, int sr) {
    Spectrum out;
    constexpr int kNfft = 256;
    if (static_cast<int>(samples.size()) < kNfft || sr <= 0) return out;
    for (double& v : samples) if (std::isnan(v)) v = 0.0;

    try {
        DataFilter::detrend(samples.data(), static_cast<int>(samples.size()),
                            static_cast<int>(DetrendOperations::LINEAR));
        PsdGuard g;
        int len = 0;
        g.psd = DataFilter::get_psd_welch(samples.data(), static_cast<int>(samples.size()),
                                          kNfft, kNfft / 2, sr,
                                          static_cast<int>(WindowOperations::HANNING), &len);
        if (len <= 0) return out;

        const double total =
            DataFilter::get_band_power(g.psd, len, kAnalysisLowHz, kAnalysisHighHz);
        if (total <= 0.0) return out;
        for (int b = 0; b < kBandCount; ++b) {
            out.bands[static_cast<std::size_t>(b)] =
                DataFilter::get_band_power(g.psd, len, kBandEdges[b][0], kBandEdges[b][1]) / total;
        }

        // Peak search starts at 4 Hz. Below that 1/f always wins and every
        // recording ever made would report a peak of 1 Hz.
        const double bin_hz = static_cast<double>(sr) / kNfft;
        double best = -1.0;
        for (int k = 1; k < len; ++k) {
            const double f = k * bin_hz;
            if (f < 4.0 || f > kAnalysisHighHz) continue;
            if (g.psd.first[k] > best) { best = g.psd.first[k]; out.peak_hz = f; }
        }
        out.valid = true;
    } catch (const BrainFlowException&) {
    }
    return out;
}

// Head motion as a scalar: three axes collapsed to a magnitude, centred on its
// own mean, then RMS over the window. This is the artifact term that decides
// whether a high-frequency EEG reading is brain or jaw muscle.
double motion_rms(const std::vector<Sample>& win, const std::vector<int>& rows) {
    if (rows.size() < 3 || win.empty()) return 0.0;
    std::vector<double> mag;
    mag.reserve(win.size());
    double sum = 0.0;
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
        if (!ok) continue;
        const double m = std::sqrt(acc);
        mag.push_back(m);
        sum += m;
    }
    if (mag.size() < 2) return 0.0;
    const double mean = sum / static_cast<double>(mag.size());
    double var = 0.0;
    for (double m : mag) var += (m - mean) * (m - mean);
    return std::sqrt(var / static_cast<double>(mag.size()));
}

// One metric card: eyebrow, a large value, a secondary line. Everything the
// operator reads at a glance is one of these.
void metric_card(const char* id, float width, float height,
                 const char* label, const char* value, theme::Rgba value_color,
                 const char* sub, theme::Rgba sub_color) {
    if (begin_card(id, ImVec2(width, height))) {
        eyebrow(label);
        ImGui::Dummy(ImVec2(1.0f, theme::kS2));

        if (fonts().metric) ImGui::PushFont(fonts().metric);
        ImGui::TextColored(v4(value_color), "%s", value);
        if (fonts().metric) ImGui::PopFont();

        ImGui::Dummy(ImVec2(1.0f, 2.0f));
        ImGui::TextColored(v4(sub_color), "%s", sub);
    }
    end_card();
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

    if (verbose) BoardShim::enable_dev_board_logger();
    else         BoardShim::disable_board_logger();

    UiShell shell("ELANORA - Muse Monitor", 1320, 800);
    if (!shell.ok()) {
        std::fprintf(stderr, "UiShell failed: %s\n", shell.error().c_str());
        return 1;
    }
    shell.set_clear_color(theme::kGround.r, theme::kGround.g, theme::kGround.b);

    MuseDevice device(synthetic ? static_cast<int>(BoardIds::SYNTHETIC_BOARD)
                                : static_cast<int>(BoardIds::MUSE_2_BOARD));
    StreamRecorder recorder;

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

    // Every number eases toward its measurement, so a value can be read while
    // it changes rather than only between changes.
    std::array<std::array<Smoothed, kBandCount>, kSensorCount> sm_band{};
    std::array<Smoothed, kSensorCount> sm_rms{};
    std::array<Smoothed, kSensorCount> sm_peak{};
    Smoothed sm_motion, sm_usable;

    std::array<Spectrum, kSensorCount> spec{};
    std::array<ChannelQuality, kSensorCount> qual{};
    double analysed_at = 0.0;
    int focus_sensor = 1;                                  // AF7

    int frames = 0;
    while (shell.begin_frame()) {
        const ImGuiIO& io = ImGui::GetIO();
        const float dt = io.DeltaTime;
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            if (device.connected()) {
                stop_polling(); recorder.stop(); device.disconnect();
                status = "Not connected"; status_error = false;
            } else {
                do_connect();
            }
        }

        const ChannelMap& ch = device.channels();
        const double t_eeg = recorder.latest_ts(BrainFlowPresets::DEFAULT_PRESET);
        const double t_imu = recorder.latest_ts(BrainFlowPresets::AUXILIARY_PRESET);
        const auto eeg_win = recorder.window(BrainFlowPresets::DEFAULT_PRESET,
                                             t_eeg - kWindowSeconds, t_eeg);
        const auto imu_win = recorder.window(BrainFlowPresets::AUXILIARY_PRESET,
                                             t_imu - kWindowSeconds, t_imu);

        // Analysis runs on its own cadence, not per frame: a 256-point Welch
        // per sensor at 60 Hz is pure waste, and the smoothing above already
        // makes the slower update read as continuous.
        const double now_t = ImGui::GetTime();
        if (now_t - analysed_at > 0.20) {
            analysed_at = now_t;
            for (int i = 0; i < kSensorCount; ++i) {
                if (i >= static_cast<int>(ch.eeg.size())) continue;
                const auto samples = channel_of(eeg_win, ch.eeg[static_cast<std::size_t>(i)]);
                spec[static_cast<std::size_t>(i)] = analyse(samples, ch.sr_eeg);
                qual[static_cast<std::size_t>(i)] = assess(samples, ch.sr_eeg);
            }
        }

        int usable = 0;
        Quality worst = Quality::Good;
        for (int i = 0; i < kSensorCount; ++i) {
            const auto& q = qual[static_cast<std::size_t>(i)];
            if (q.q != Quality::Bad) ++usable;
            if (static_cast<int>(q.q) > static_cast<int>(worst)) worst = q.q;
            sm_rms[static_cast<std::size_t>(i)].set(q.rms_uv, dt);
            sm_peak[static_cast<std::size_t>(i)].set(spec[static_cast<std::size_t>(i)].peak_hz, dt);
            for (int b = 0; b < kBandCount; ++b) {
                sm_band[static_cast<std::size_t>(i)][static_cast<std::size_t>(b)]
                    .set(spec[static_cast<std::size_t>(i)].bands[static_cast<std::size_t>(b)], dt);
            }
        }
        sm_usable.set(static_cast<double>(usable), dt);
        sm_motion.set(motion_rms(imu_win, ch.accel), dt);

        // ---- frame ---------------------------------------------------------
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS5, theme::kS5));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

        const float gap = theme::kS3;

        // ---- header --------------------------------------------------------
        if (fonts().subhead) ImGui::PushFont(fonts().subhead);
        ImGui::TextUnformatted("Elanora");
        if (fonts().subhead) ImGui::PopFont();

        ImGui::SameLine(0.0f, theme::kS3);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);
        // Connection status is always visible, per the real-time monitoring
        // checklist: an operator must never have to guess whether data is live.
        status_pill(status.c_str(),
                    status_error ? theme::kBad
                                 : device.connected() ? theme::kGood : theme::kMuted);

        ImGui::SameLine(0.0f, theme::kS3);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f);
        if (!device.connected()) {
            ImGui::SetNextItemWidth(190.0f);
            ImGui::InputTextWithHint("##id", "serial or MAC",
                                     device_id.data(), device_id.size());
            ImGui::SameLine(0.0f, theme::kS2);
            if (ImGui::Button("Connect")) do_connect();
        } else if (ImGui::Button("Disconnect")) {
            stop_polling(); recorder.stop(); device.disconnect();
            status = "Not connected"; status_error = false;
        }

        ImGui::SameLine();
        right_align(360.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5.0f);

        // Data freshness. A dashboard of numbers gives no clue whether it is
        // live or frozen on the last packet before a BLE stall, so say so.
        const double age = (device.connected() && t_eeg > 0.0)
                               ? (recorder.latest_ts(BrainFlowPresets::DEFAULT_PRESET) > 0.0
                                      ? 0.0 : 1e9)
                               : 1e9;
        (void)age;
        static double last_seen_ts = 0.0;
        static double last_seen_at = 0.0;
        if (t_eeg > last_seen_ts) { last_seen_ts = t_eeg; last_seen_at = now_t; }
        const double stale = device.connected() ? (now_t - last_seen_at) : 0.0;

        if (device.connected() && stale > 1.0) {
            text_colored(theme::kBad, "NO DATA %.0fs", stale);
            ImGui::SameLine(0.0f, theme::kS3);
        }
        text_mono(theme::kFaint, "EEG %d Hz   IMU %d Hz   space to connect",
                  ch.sr_eeg, ch.sr_imu);

        ImGui::Dummy(ImVec2(1.0f, theme::kS1));

        // ---- metric row ----------------------------------------------------
        const float avail  = ImGui::GetContentRegionAvail().x;
        const float card_w = (avail - gap * 3.0f) / 4.0f;
        const float card_h = 136.0f;

        const auto& focus = spec[static_cast<std::size_t>(focus_sensor)];
        int dom = 0;
        for (int b = 1; b < kBandCount; ++b) {
            if (focus.bands[static_cast<std::size_t>(b)] >
                focus.bands[static_cast<std::size_t>(dom)]) dom = b;
        }

        char buf[64], sub[96];
        std::snprintf(sub, sizeof(sub), "%.2f on %s",
                      sm_band[static_cast<std::size_t>(focus_sensor)]
                             [static_cast<std::size_t>(dom)].value,
                      electrode_name(static_cast<SensorId>(focus_sensor)));
        metric_card("##m1", card_w, card_h, "Dominant band",
                    focus.valid ? band_name(static_cast<Band>(dom)) : "--",
                    focus.valid ? theme::band_color(dom) : theme::kFaint,
                    focus.valid ? sub : "no signal", theme::kMuted);

        ImGui::SameLine(0.0f, gap);
        std::snprintf(buf, sizeof(buf), "%.0f/4", sm_usable.value);
        std::snprintf(sub, sizeof(sub), "%s",
                      usable == kSensorCount ? "all electrodes good"
                      : worst == Quality::Bad ? "check headband"
                                              : "one degraded");
        metric_card("##m2", card_w, card_h, "Electrodes", buf,
                    usable == kSensorCount ? theme::kGood
                    : usable >= 3          ? theme::kWarn
                                           : theme::kBad,
                    sub, theme::kMuted);

        ImGui::SameLine(0.0f, gap);
        const bool still = sm_motion.value < 0.05;
        std::snprintf(buf, sizeof(buf), "%.3f", sm_motion.value);
        metric_card("##m3", card_w, card_h, "Head motion", buf,
                    still ? theme::kGood : theme::kWarn,
                    still ? "still" : "moving, artifacts likely", theme::kMuted);

        ImGui::SameLine(0.0f, gap);
        std::snprintf(buf, sizeof(buf), "%.1f",
                      sm_peak[static_cast<std::size_t>(focus_sensor)].value);
        std::snprintf(sub, sizeof(sub), "Hz on %s",
                      electrode_name(static_cast<SensorId>(focus_sensor)));
        metric_card("##m4", card_w, card_h, "Spectral peak",
                    focus.valid ? buf : "--",
                    focus.valid ? theme::kText : theme::kFaint, sub, theme::kMuted);

        ImGui::Dummy(ImVec2(1.0f, theme::kS1));

        // ---- band matrix ---------------------------------------------------
        const float table_h = ImGui::GetContentRegionAvail().y - 4.0f;
        if (begin_card("##matrix", ImVec2(0, table_h))) {
            eyebrow("Relative band power");
            ImGui::SameLine();
            right_align(190.0f);
            text_mono(theme::kFaint, "click a row to focus");
            ImGui::Dummy(ImVec2(1.0f, theme::kS3));

            // Wider label and right-hand columns: the band columns previously
            // took luxurious width while amplitude and peak were squeezed
            // against the edge, which read as an unbalanced table.
            const float lbl_w  = 104.0f;
            const float rms_w  = 132.0f;
            const float peak_w = 108.0f;
            const float grid_w = ImGui::GetContentRegionAvail().x - lbl_w - rms_w - peak_w;
            const float col_w  = grid_w / kBandCount;

            {
                const ImVec2 o = ImGui::GetCursorScreenPos();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
                for (int b = 0; b < kBandCount; ++b) {
                    const float x = o.x + lbl_w + col_w * b;
                    // A dot carries the band's hue; the label stays neutral, so
                    // the header is legible instead of a row of colours.
                    dl->AddCircleFilled(ImVec2(x + 3.0f, o.y + 6.0f), 3.0f,
                                        ImGui::GetColorU32(v4(theme::band_color(b))));
                    dl->AddText(ImVec2(x + 12.0f, o.y),
                                ImGui::GetColorU32(v4(theme::kMuted)),
                                band_name(static_cast<Band>(b)));
                }
                dl->AddText(ImVec2(o.x + lbl_w + grid_w, o.y),
                            ImGui::GetColorU32(v4(theme::kMuted)), "AMPLITUDE");
                dl->AddText(ImVec2(o.x + lbl_w + grid_w + rms_w, o.y),
                            ImGui::GetColorU32(v4(theme::kMuted)), "PEAK");
                if (fonts().eyebrow) ImGui::PopFont();
                ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight() + theme::kS3));
            }

            const float body_h = ImGui::GetContentRegionAvail().y - 4.0f;
            const float row_h  = body_h / kSensorCount;

            for (int r = 0; r < kSensorCount; ++r) {
                const ImVec2 ro = ImGui::GetCursorScreenPos();
                const float row_w = ImGui::GetContentRegionAvail().x;

                char rid[16];
                std::snprintf(rid, sizeof(rid), "##r%d", r);
                ImGui::SetCursorScreenPos(ro);
                ImGui::InvisibleButton(rid, ImVec2(row_w, row_h));
                if (ImGui::IsItemClicked()) focus_sensor = r;
                const bool hot = ImGui::IsItemHovered() || r == focus_sensor;

                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (hot) {
                    dl->AddRectFilled(ImVec2(ro.x - theme::kS3, ro.y),
                                      ImVec2(ro.x + row_w, ro.y + row_h - 6.0f),
                                      ImGui::GetColorU32(v4(theme::kRaised)),
                                      theme::kRadiusSm);
                }

                int rdom = 0;
                for (int b = 1; b < kBandCount; ++b) {
                    if (spec[static_cast<std::size_t>(r)].bands[static_cast<std::size_t>(b)] >
                        spec[static_cast<std::size_t>(r)].bands[static_cast<std::size_t>(rdom)])
                        rdom = b;
                }

                const float line_h = ImGui::GetTextLineHeight();
                // Content is centred in the row rather than pinned to the top,
                // so the generous row height reads as breathing room instead of
                // as a value floating in an empty box.
                const float ty = ro.y + row_h * 0.5f - 26.0f;

                if (fonts().subhead) ImGui::PushFont(fonts().subhead);
                dl->AddText(ImVec2(ro.x, ty + 2.0f),
                            ImGui::GetColorU32(v4(r == focus_sensor ? theme::kText
                                                                    : theme::kDim)),
                            electrode_name(static_cast<SensorId>(r)));
                if (fonts().subhead) ImGui::PopFont();

                // Quality sits beside the electrode name, on its baseline --
                // it belongs to the sensor, not underneath the whole row.
                const auto& q = qual[static_cast<std::size_t>(r)];
                if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
                dl->AddText(ImVec2(ro.x, ty + line_h + 12.0f),
                            ImGui::GetColorU32(v4(quality_color(q.q))),
                            q.flat ? "no contact" : q.railed ? "saturated" : quality_name(q.q));
                if (fonts().eyebrow) ImGui::PopFont();

                for (int b = 0; b < kBandCount; ++b) {
                    const double val = sm_band[static_cast<std::size_t>(r)]
                                              [static_cast<std::size_t>(b)].value;
                    const bool is_dom = (b == rdom) && spec[static_cast<std::size_t>(r)].valid;
                    const float cx = ro.x + lbl_w + col_w * b;

                    char txt[16];
                    std::snprintf(txt, sizeof(txt), "%.2f", val);
                    if (fonts().monoBig) ImGui::PushFont(fonts().monoBig);
                    dl->AddText(ImVec2(cx, ty),
                                ImGui::GetColorU32(v4(is_dom ? theme::kText : theme::kFaint)),
                                txt);
                    if (fonts().monoBig) ImGui::PopFont();

                    // A bar under each value. The number gives the exact
                    // figure; the bar makes five of them comparable without
                    // reading any of them, which is what a matrix is for.
                    const float bw = col_w - theme::kS4;
                    const float by = ty + line_h + 18.0f;
                    dl->AddRectFilled(ImVec2(cx, by), ImVec2(cx + bw, by + 3.0f),
                                      ImGui::GetColorU32(v4(theme::kLine)), 1.5f);
                    if (val > 0.001) {
                        const float fill = bw * static_cast<float>(std::clamp(val, 0.0, 1.0));
                        dl->AddRectFilled(
                            ImVec2(cx, by), ImVec2(cx + fill, by + 3.0f),
                            ImGui::GetColorU32(ImVec4(
                                theme::band_color(b).r, theme::band_color(b).g,
                                theme::band_color(b).b, is_dom ? 1.0f : 0.35f)),
                            1.5f);
                    }
                }

                if (fonts().mono) ImGui::PushFont(fonts().mono);
                char rms[24];
                std::snprintf(rms, sizeof(rms), "%.1f uV",
                              sm_rms[static_cast<std::size_t>(r)].value);
                dl->AddText(ImVec2(ro.x + lbl_w + grid_w, ty + 6.0f),
                            ImGui::GetColorU32(v4(quality_color(q.q))), rms);
                char pk[24];
                std::snprintf(pk, sizeof(pk), "%.1f Hz",
                              sm_peak[static_cast<std::size_t>(r)].value);
                dl->AddText(ImVec2(ro.x + lbl_w + grid_w + rms_w, ty + 6.0f),
                            ImGui::GetColorU32(v4(theme::kMuted)), pk);
                if (fonts().mono) ImGui::PopFont();

                // Hairline between rows, drawn last so the hover fill sits
                // under it rather than cutting it.
                if (r + 1 < kSensorCount) {
                    dl->AddLine(ImVec2(ro.x, ro.y + row_h - 1.0f),
                                ImVec2(ro.x + row_w, ro.y + row_h - 1.0f),
                                ImGui::GetColorU32(v4(theme::kLine)), 1.0f);
                }

                ImGui::SetCursorScreenPos(ImVec2(ro.x, ro.y + row_h));
            }
        }
        end_card();

        ImGui::End();
        ImGui::PopStyleVar(2);
        shell.end_frame();

        ++frames;
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
