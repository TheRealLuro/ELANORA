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
//   muse_monitor --demo          lifelike synthesised EEG, for judging the UI
//   muse_monitor --autoconnect   connect on launch
//   muse_monitor --frames N      render N frames then exit (automation)
//   muse_monitor --verbose       leave BrainFlow's stderr logging on
//   muse_monitor --screenshot P  save the final frame to PNG at P
//   muse_monitor --collector     open on the Collector tab
//   muse_monitor --autostart     begin a trial immediately (automation)

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

#include "elanora/collector/view.hpp"
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

// ---------------------------------------------------------------------------
// Demo signal.
//
// BrainFlow's synthetic board emits a pure sine per channel, so every band
// power it produces is constant -- nothing on this dashboard can be evaluated
// against it, because nothing moves.
//
// This generates something that behaves like EEG instead: pink-ish 1/f noise
// for the background, alpha arriving in spindles rather than as a constant
// tone, occasional eye blinks on the frontal channels, and slow drift in the
// electrode amplitudes. Clearly labelled as demo data -- it is for judging the
// interface, never for judging the science.
// ---------------------------------------------------------------------------
class DemoSource {
public:
    DemoSource() {
        for (int c = 0; c < kSensorCount; ++c) {
            auto& ch = ch_[static_cast<std::size_t>(c)];
            ch.alpha_hz  = 9.2 + 0.9 * c * 0.25;
            ch.next_burst = 0.6 + 0.9 * c;
            ch.base_uv   = 12.0 + 4.0 * c;
            ch.frontal   = (c == 1 || c == 2);
            for (double& r : ch.pink) r = urand() * 2.0 - 1.0;
        }
    }

    // Produces one window ending "now", in the Sample shape the rest of the
    // app already consumes, so nothing downstream needs a demo branch.
    std::vector<Sample> window(double seconds, int sr) {
        const int n = static_cast<int>(seconds * sr);
        std::vector<Sample> out;
        out.reserve(static_cast<std::size_t>(n));
        const double dt = 1.0 / sr;

        for (int i = 0; i < n; ++i) {
            t_ += dt;
            Sample smp;
            smp.ts = t_;
            smp.values.assign(static_cast<std::size_t>(kSensorCount) + 2u, 0.0);
            smp.values[0] = 0.0;
            for (int c = 0; c < kSensorCount; ++c) {
                smp.values[static_cast<std::size_t>(c) + 1u] = step_channel(c, dt);
            }
            smp.values[static_cast<std::size_t>(kSensorCount) + 1u] = t_;
            out.push_back(std::move(smp));
        }
        return out;
    }

    double motion() const { return 0.012 + 0.010 * std::sin(t_ * 0.23); }

private:
    struct Ch {
        double pink[6]{};
        int    counter = 0;
        double running = 0.0;
        double alpha_phase = 0.0, alpha_hz = 10.0;
        double burst_t = -1.0, burst_len = 0.0, next_burst = 1.0;
        double blink_t = -1.0, next_blink = 3.0;
        double base_uv = 14.0;
        bool   frontal = false;
    };

    static double urand() {
        static unsigned long long s = 0x9E3779B97F4A7C15ull;
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return static_cast<double>(s % 100000) / 100000.0;
    }

    // Voss-McCartney: octave-spaced random walks summed. Real EEG has a 1/f
    // spectrum; white noise looks obviously wrong, with no slow wander at all.
    double pink_next(Ch& c) {
        c.counter++;
        for (int i = 0; i < 6; ++i) {
            if (c.counter % (1 << i) == 0) {
                c.running -= c.pink[i];
                c.pink[i] = urand() * 2.0 - 1.0;
                c.running += c.pink[i];
                break;
            }
        }
        return c.running / 6.0 + (urand() - 0.5) * 0.25;
    }

    double step_channel(int idx, double dt) {
        Ch& c = ch_[static_cast<std::size_t>(idx)];

        // Alpha in spindles: 1-2 s bursts, then nothing. A constant alpha tone
        // is the classic tell of a faked EEG trace.
        if (c.burst_t < 0.0 && t_ > c.next_burst) {
            c.burst_t = 0.0;
            c.burst_len = 0.9 + urand() * 1.6;
            c.next_burst = t_ + c.burst_len + 0.8 + urand() * 3.5;
        }
        double burst = 0.0;
        if (c.burst_t >= 0.0) {
            c.burst_t += dt;
            const double pr = c.burst_t / c.burst_len;
            if (pr >= 1.0) c.burst_t = -1.0;
            else burst = std::sin(3.14159265358979 * pr);
        }

        double blink = 0.0;
        if (c.frontal) {
            if (c.blink_t < 0.0 && t_ > c.next_blink) {
                c.blink_t = 0.0;
                c.next_blink = t_ + 3.0 + urand() * 6.0;
            }
            if (c.blink_t >= 0.0) {
                c.blink_t += dt;
                const double b = c.blink_t / 0.28;
                if (b >= 1.0) c.blink_t = -1.0;
                else blink = -std::sin(3.14159265358979 * b) * std::exp(-b * 1.1);
            }
        }

        c.alpha_phase += 6.28318530717958 * c.alpha_hz * dt;

        // Slow amplitude drift, the way a settling headband behaves.
        const double drift = 1.0 + 0.18 * std::sin(t_ * 0.07 + idx);
        // Scaled so RMS lands in the 10-30 uV band real resting EEG occupies,
        // and the alpha burst is weighted well below the 1/f background so
        // dominance actually changes hands between bands over time rather than
        // alpha winning every window.
        return (pink_next(c) * 2.6
                + burst * std::sin(c.alpha_phase) * 1.05
                + blink * 4.0) * c.base_uv * drift * 2.2;
    }

    std::array<Ch, kSensorCount> ch_{};
    double t_ = 1.0e6;
};

// Electrode card: a head seen from above with the four sensors in their real
// positions, each coloured by quality.
//
// A ring reading "4/4" with a "4" inside it says the same thing three times and
// still does not say WHICH electrode is bad -- which is the only thing you
// actually need when one goes. Position is the answer, so the card shows
// position.
void electrode_card(const char* id, float width, float height,
                    const std::array<ChannelQuality, kSensorCount>& qual,
                    int usable, Quality worst) {
    if (begin_card(id, ImVec2(width, height))) {
        const ImVec2 p0 = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        const float cx = p0.x + w * 0.5f;

        if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
        const float eyebrow_h = ImGui::GetTextLineHeight();
        text_centered(ImVec2(cx, p0.y + theme::kS4 + eyebrow_h * 0.5f), "Electrodes",
                      theme::kMuted);
        if (fonts().eyebrow) ImGui::PopFont();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = 34.0f;
        const float cy = p0.y + theme::kS4 + eyebrow_h + theme::kS4 + r;

        // Head outline, nose at the top so left and right are unambiguous.
        dl->AddCircleFilled(ImVec2(cx, cy), r, ImGui::GetColorU32(v4(theme::kGround)), 48);
        dl->AddCircle(ImVec2(cx, cy), r, ImGui::GetColorU32(v4(theme::kLineHi)), 48, 1.4f);
        dl->AddTriangleFilled(ImVec2(cx - 5.0f, cy - r + 1.0f),
                              ImVec2(cx + 5.0f, cy - r + 1.0f),
                              ImVec2(cx, cy - r - 6.0f),
                              ImGui::GetColorU32(v4(theme::kLineHi)));

        // Muse 2 positions: AF7/AF8 on the forehead, TP9/TP10 at the ears.
        struct Pos { float x, y; };
        static const Pos kPos[kSensorCount] = {
            {-0.92f,  0.10f},   // TP9  left ear
            {-0.48f, -0.62f},   // AF7  left forehead
            { 0.48f, -0.62f},   // AF8  right forehead
            { 0.92f,  0.10f}    // TP10 right ear
        };

        for (int i = 0; i < kSensorCount; ++i) {
            const auto& q = qual[static_cast<std::size_t>(i)];
            const theme::Rgba col = quality_color(q.q);
            const ImVec2 pt(cx + kPos[i].x * r, cy + kPos[i].y * r);

            // A bad sensor pulses, so it is found without reading anything.
            const float pulse = (q.q == Quality::Bad)
                ? 0.55f + 0.45f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f)
                : 1.0f;
            dl->AddCircleFilled(pt, 9.0f, ImGui::GetColorU32(
                ImVec4(col.r, col.g, col.b, 0.22f * pulse)), 20);
            dl->AddCircleFilled(pt, 5.5f, ImGui::GetColorU32(
                ImVec4(col.r, col.g, col.b, pulse)), 20);

            if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
            const char* nm = electrode_name(static_cast<SensorId>(i));
            const float tw = ImGui::CalcTextSize(nm).x;
            const float lx = pt.x + (kPos[i].x < 0.0f ? -(tw + 13.0f) : 13.0f);
            dl->AddText(ImVec2(lx, pt.y - ImGui::GetTextLineHeight() * 0.5f),
                        ImGui::GetColorU32(v4(q.q == Quality::Good ? theme::kMuted : col)),
                        nm);
            if (fonts().eyebrow) ImGui::PopFont();
        }

        char v[16];
        std::snprintf(v, sizeof(v), "%d/4", usable);
        if (fonts().metric) ImGui::PushFont(fonts().metric);
        const float vh = ImGui::GetTextLineHeight();
        text_centered(ImVec2(cx, cy + r + theme::kS4 + vh * 0.5f), v,
                      usable == kSensorCount ? theme::kGood
                      : usable >= 3          ? theme::kWarn
                                             : theme::kBad);
        if (fonts().metric) ImGui::PopFont();

        text_centered(ImVec2(cx, cy + r + theme::kS4 + vh + theme::kS1 +
                                 ImGui::GetTextLineHeight() * 0.5f),
                      usable == kSensorCount ? "all seated"
                      : worst == Quality::Bad ? "reseat headband"
                                              : "one degraded",
                      theme::kMuted);
    }
    end_card();   // unconditional, like ImGui::EndChild
}

// One card shape for the entire metric row: a centred stack of eyebrow, ring,
// headline and caption.
//
// Left-aligned content in a card leaves a dead corner, and four cards each
// leaving it in a different place is what made the row look unresolved. A
// centred stack is symmetric, so nothing has to be balanced by eye.
//
// `fraction` is what the ring sweeps; `value` is the text that means something
// to a person. They are deliberately separate -- 9.6 Hz is meaningless as an
// arc until you know it is 9.6 out of a 45 Hz range.
void metric_card(const char* id, float width, float height,
                 const char* label, const char* value, theme::Rgba value_color,
                 const char* sub, double fraction, const char* ring_text,
                 bool valid) {
    if (begin_card(id, ImVec2(width, height))) {
        const ImVec2 p0 = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        const float cx = p0.x + w * 0.5f;

        // Eyebrow, centred.
        if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
        const float eyebrow_h = ImGui::GetTextLineHeight();
        text_centered(ImVec2(cx, p0.y + theme::kS4 + eyebrow_h * 0.5f), label,
                      theme::kMuted);
        if (fonts().eyebrow) ImGui::PopFont();

        const float r = 34.0f;
        const float ring_cy = p0.y + theme::kS4 + eyebrow_h + theme::kS4 + r;
        ring_gauge(ImVec2(cx, ring_cy), r, 9.0f, valid ? fraction : 0.0,
                   valid ? value_color : theme::kFaint, theme::kGround);

        if (ring_text != nullptr && ring_text[0] != '\0') {
            if (fonts().body) ImGui::PushFont(fonts().body);
            text_centered(ImVec2(cx, ring_cy), ring_text,
                          valid ? theme::kDim : theme::kFaint);
            if (fonts().body) ImGui::PopFont();
        }

        if (fonts().metric) ImGui::PushFont(fonts().metric);
        const float vh = ImGui::GetTextLineHeight();
        text_centered(ImVec2(cx, ring_cy + r + theme::kS4 + vh * 0.5f), value,
                      valid ? value_color : theme::kFaint);
        if (fonts().metric) ImGui::PopFont();

        const float sub_y = ring_cy + r + theme::kS4 + vh + theme::kS1;
        text_centered(ImVec2(cx, sub_y + ImGui::GetTextLineHeight() * 0.5f), sub,
                      theme::kMuted);
    }
    end_card();   // unconditional, like ImGui::EndChild
}

}  // namespace

int main(int argc, char** argv) {
    bool synthetic = false, autoconnect = false, verbose = false, demo = false;
    int max_frames = -1;
    const char* shot_path = nullptr;
    bool start_collector = false;
    bool autostart = false;
    Transport transport = Transport::NativeBle;
    std::string bled_port;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--synthetic") == 0)        synthetic = true;
        else if (std::strcmp(argv[i], "--demo") == 0)        demo = true;
        else if (std::strcmp(argv[i], "--autoconnect") == 0) autoconnect = true;
        else if (std::strcmp(argv[i], "--verbose") == 0)     verbose = true;
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            max_frames = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            shot_path = argv[++i];
        else if (std::strcmp(argv[i], "--collector") == 0) start_collector = true;
        else if (std::strcmp(argv[i], "--autostart") == 0) autostart = true;
        else if (std::strcmp(argv[i], "--bled") == 0) {
            transport = Transport::BledDongle;
            // The port is optional so --bled alone picks the only port there
            // is, which is the common case with one dongle plugged in.
            if (i + 1 < argc && argv[i + 1][0] != '-') bled_port = argv[++i];
        }
    }

    if (transport == Transport::BledDongle && bled_port.empty()) {
        const std::vector<std::string> ports = list_serial_ports();
        if (ports.size() == 1) bled_port = ports.front();
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

    std::vector<std::string> serial_ports = list_serial_ports();

    auto do_connect = [&] {
        ConnectRequest req;
        req.transport     = transport;
        req.serial_or_mac = device_id.data();
        req.serial_port   = bled_port;
        std::string err;
        if (device.connect(req, err)) {
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
    if (autoconnect && !demo) do_connect();
    if (demo) { status = "Demo data"; status_error = false; }

    // Every number eases toward its measurement, so a value can be read while
    // it changes rather than only between changes.
    std::array<std::array<Smoothed, kBandCount>, kSensorCount> sm_band{};
    std::array<Smoothed, kSensorCount> sm_rms{};
    std::array<Smoothed, kSensorCount> sm_peak{};
    Smoothed sm_motion, sm_usable;

    DemoSource demo_source;
    collector::CollectorState collector_state;
    int tab = start_collector ? 1 : 0;                    // 0 monitor, 1 collector
    if (autostart) {
        // Runs a trial without a click so the running view can be exercised in
        // automation. Speed is raised so a survey is reached in seconds.
        collector_state.runner.start(collector_state.preview_schedule(),
                                     collector_state.durations);
        collector_state.speed = 30.0;
    }
    std::array<Spectrum, kSensorCount> spec{};
    std::array<ChannelQuality, kSensorCount> qual{};
    double analysed_at = 0.0;
    int focus_sensor = 1;                                  // AF7

    int frames = 0;
    while (shell.begin_frame()) {
        const ImGuiIO& io = ImGui::GetIO();
        const float dt = io.DeltaTime;
        if (tab == 0 && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space)) {
            if (device.connected()) {
                stop_polling(); recorder.stop(); device.disconnect();
                status = "Not connected"; status_error = false;
            } else {
                do_connect();
            }
        }

        ChannelMap ch = device.channels();
        double t_eeg = recorder.latest_ts(BrainFlowPresets::DEFAULT_PRESET);
        const double t_imu = recorder.latest_ts(BrainFlowPresets::AUXILIARY_PRESET);
        std::vector<Sample> eeg_win;
        const auto imu_win = recorder.window(BrainFlowPresets::AUXILIARY_PRESET,
                                             t_imu - kWindowSeconds, t_imu);
        if (demo) {
            // Demo data flows through the identical analysis path, so what the
            // interface shows here is what it will show on a real headset.
            ch.eeg = {1, 2, 3, 4};
            ch.sr_eeg = 256;
            eeg_win = demo_source.window(kWindowSeconds, ch.sr_eeg);
            t_eeg = eeg_win.empty() ? 0.0 : eeg_win.back().ts;
        } else {
            eeg_win = recorder.window(BrainFlowPresets::DEFAULT_PRESET,
                                      t_eeg - kWindowSeconds, t_eeg);
        }

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
        sm_motion.set(demo ? demo_source.motion() : motion_rms(imu_win, ch.accel), dt);

        // ---- frame ---------------------------------------------------------
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS6, theme::kS5));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

        // Ambient light behind the glass. Placed off the working area so the
        // panels sit over a gradient rather than a flat field -- translucency
        // over a uniform colour shows nothing at all.
        {
            const ImVec2 wp = vp->WorkPos;
            const ImVec2 ws = vp->WorkSize;
            // Centres pushed below the header band and alphas cut: at the
            // first values the teal glow washed out the header text entirely,
            // which is a contrast failure, not a style choice.
            soft_glow(ImVec2(wp.x + ws.x * 0.14f, wp.y + ws.y * 0.34f),
                      ws.y * 0.78f, theme::kAccent, 0.032f);
            soft_glow(ImVec2(wp.x + ws.x * 0.86f, wp.y + ws.y * 0.30f),
                      ws.y * 0.66f, theme::kAlpha, 0.022f);
            soft_glow(ImVec2(wp.x + ws.x * 0.50f, wp.y + ws.y * 1.02f),
                      ws.y * 0.70f, theme::kDelta, 0.026f);
        }

        const float gap = theme::kS4;

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

        ImGui::SameLine(0.0f, theme::kS4);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);
        static const char* kTabs[] = {"Monitor", "Collector"};
        segmented("##tab", kTabs, 2, &tab, 196.0f);

        ImGui::SameLine(0.0f, theme::kS4);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
        if (!device.connected()) {
            // Transport first: it decides whether the field beside it is a
            // headset name or a COM port, and an operator who picks the wrong
            // one gets a 15 s discovery timeout before finding out.
            int tsel = (transport == Transport::BledDongle) ? 1 : 0;
            static const char* kTransports[] = {"Bluetooth", "USB dongle"};
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
            if (segmented("##transport", kTransports, 2, &tsel, 172.0f)) {
                transport = tsel == 1 ? Transport::BledDongle : Transport::NativeBle;
                if (transport == Transport::BledDongle && bled_port.empty()) {
                    serial_ports = list_serial_ports();
                    if (!serial_ports.empty()) bled_port = serial_ports.front();
                }
            }
            ImGui::SameLine(0.0f, theme::kS2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);

            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputTextWithHint("##id", "serial or MAC",
                                     device_id.data(), device_id.size());

            if (transport == Transport::BledDongle) {
                ImGui::SameLine(0.0f, theme::kS2);
                ImGui::SetNextItemWidth(112.0f);
                const char* preview = bled_port.empty() ? "port" : bled_port.c_str();
                if (ImGui::BeginCombo("##port", preview)) {
                    // Re-enumerated on open, not once at startup: a dongle
                    // plugged in after launch must appear without a restart.
                    serial_ports = list_serial_ports();
                    if (serial_ports.empty()) {
                        ImGui::TextUnformatted("no serial ports");
                    }
                    for (const std::string& p : serial_ports) {
                        if (ImGui::Selectable(p.c_str(), p == bled_port)) bled_port = p;
                    }
                    ImGui::EndCombo();
                }
            }

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
        text_mono(theme::kMuted, "EEG %d Hz   IMU %d Hz   space to connect",
                  ch.sr_eeg, ch.sr_imu);

        ImGui::Dummy(ImVec2(1.0f, theme::kS1));

        if (tab == 1) {
            // The collector needs live electrode state so a headset problem is
            // caught in the round it happens, not at analysis.
            collector::draw_collector(collector_state, qual,
                                      device.connected() ? &recorder : nullptr,
                                      device.connected() ? &ch : nullptr, dt);
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
            continue;
        }

        // ---- metric row ----------------------------------------------------
        const float avail  = ImGui::GetContentRegionAvail().x;
        const float card_w = (avail - gap * 3.0f) / 4.0f;
        const float card_h = 214.0f;

        const auto& focus = spec[static_cast<std::size_t>(focus_sensor)];
        int dom = 0;
        for (int b = 1; b < kBandCount; ++b) {
            if (focus.bands[static_cast<std::size_t>(b)] >
                focus.bands[static_cast<std::size_t>(dom)]) dom = b;
        }

        char buf[64], sub[96], ring[16];

        const double dom_v = sm_band[static_cast<std::size_t>(focus_sensor)]
                                    [static_cast<std::size_t>(dom)].value;
        std::snprintf(ring, sizeof(ring), "%.2f", focus.valid ? dom_v : 0.0);
        std::snprintf(sub, sizeof(sub), "on %s",
                      electrode_name(static_cast<SensorId>(focus_sensor)));
        metric_card("##m1", card_w, card_h, "Dominant band",
                    focus.valid ? band_name(static_cast<Band>(dom)) : "--",
                    focus.valid ? theme::band_color(dom) : theme::kFaint,
                    sub, dom_v, ring, focus.valid);

        ImGui::SameLine(0.0f, gap);
        electrode_card("##m2", card_w, card_h, qual, usable, worst);

        ImGui::SameLine(0.0f, gap);
        const bool still = sm_motion.value < 0.05;
        std::snprintf(buf, sizeof(buf), "%.3f", sm_motion.value);
        // Normalised against 0.2 rms, which is well into "obviously moving".
        // A ring needs a ceiling, and an unbounded quantity has to be given one
        // explicitly rather than left to auto-scale into meaninglessness.
        metric_card("##m3", card_w, card_h, "Head motion", buf,
                    still ? theme::kGood : theme::kWarn,
                    still ? "still" : "artifacts likely",
                    std::clamp(sm_motion.value / 0.2, 0.0, 1.0),
                    still ? "ok" : "!", true);

        ImGui::SameLine(0.0f, gap);
        const double peak_hz = sm_peak[static_cast<std::size_t>(focus_sensor)].value;
        std::snprintf(buf, sizeof(buf), "%.1f Hz", peak_hz);
        std::snprintf(sub, sizeof(sub), "on %s",
                      electrode_name(static_cast<SensorId>(focus_sensor)));
        // No ring caption. The ring is a gauge of where the peak sits in the
        // 1-45 Hz analysis span and the headline below already carries the
        // number; a bare "Hz" in the middle labelled nothing.
        metric_card("##m4", card_w, card_h, "Spectral peak",
                    focus.valid ? buf : "--", theme::kAccent, sub,
                    std::clamp(peak_hz / kAnalysisHighHz, 0.0, 1.0), nullptr, focus.valid);

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
                    const char* nm = band_name(static_cast<Band>(b));
                    const float ccx = o.x + lbl_w + col_w * b + col_w * 0.5f - theme::kS3;
                    const float tw = ImGui::CalcTextSize(nm).x;
                    // A dot carries the band's hue; the label stays neutral, so
                    // the header is legible instead of a row of colours. Both
                    // centre over the column the values sit in.
                    dl->AddCircleFilled(ImVec2(ccx - tw * 0.5f - 8.0f, o.y + 6.0f), 3.0f,
                                        ImGui::GetColorU32(v4(theme::band_color(b))));
                    dl->AddText(ImVec2(ccx - tw * 0.5f, o.y),
                                ImGui::GetColorU32(v4(theme::kMuted)), nm);
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

                // Status as a symbol rather than a word. A check is recognised
                // before "good" is read, which matters when the question is
                // "is this electrode seated" and both your hands are busy.
                const auto& q = qual[static_cast<std::size_t>(r)];
                const int level = (q.q == Quality::Good) ? 0
                                : (q.q == Quality::Fair) ? 1 : 2;
                status_glyph(ImVec2(ro.x + 9.0f, ty + line_h + 16.0f), 9.0f,
                             level, quality_color(q.q));
                // The specific failure still gets words, because "no contact"
                // and "saturated" call for different fixes.
                if (level != 0) {
                    if (fonts().eyebrow) ImGui::PushFont(fonts().eyebrow);
                    dl->AddText(ImVec2(ro.x + 24.0f, ty + line_h + 10.0f),
                                ImGui::GetColorU32(v4(quality_color(q.q))),
                                q.flat ? "no contact" : q.railed ? "saturated" : "weak");
                    if (fonts().eyebrow) ImGui::PopFont();
                }

                for (int b = 0; b < kBandCount; ++b) {
                    const double val = sm_band[static_cast<std::size_t>(r)]
                                              [static_cast<std::size_t>(b)].value;
                    const bool is_dom = (b == rdom) && spec[static_cast<std::size_t>(r)].valid;
                    const float cx = ro.x + lbl_w + col_w * b;

                    char txt[16];
                    std::snprintf(txt, sizeof(txt), "%.2f", val);
                    if (fonts().monoBig) ImGui::PushFont(fonts().monoBig);
                    text_centered(ImVec2(cx + col_w * 0.5f - theme::kS3,
                                         ty + ImGui::GetTextLineHeight() * 0.5f),
                                  txt, is_dom ? theme::kText : theme::kMuted);
                    if (fonts().monoBig) ImGui::PopFont();

                    // A bar under each value. The number gives the exact
                    // figure; the bar makes five of them comparable without
                    // reading any of them, which is what a matrix is for.
                    //
                    // 10px and rounded, with a gradient along its length. The
                    // previous 3px hairline was too thin to compare at a
                    // glance, which defeated the point of drawing it at all.
                    // Wider and taller: the bar should be the second thing you
                    // see after the number, not a detail under it.
                    const float bw = col_w - theme::kS5;
                    const float by = ty + line_h + 18.0f;
                    value_bar(ImVec2(cx + (col_w - theme::kS3 - bw) * 0.5f, by),
                              ImVec2(bw, 13.0f), val, theme::band_color(b), is_dom);
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
        end_card();   // unconditional, like ImGui::EndChild

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
