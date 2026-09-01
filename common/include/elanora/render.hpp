#pragma once
//
// Instrument rendering helpers shared by the four applications.
//
// The trace renderer here is the difference between a plot and an instrument.
// Each rule below was established against synthesised data in the design
// prototype; a naive implementation fails all of them once real acquisition
// data arrives.
//
// IMPORTANT: everything in this header is DISPLAY ONLY. Numbers that reach a
// dataset are computed from the raw ring buffer, never from a filtered display
// copy -- otherwise band powers would silently depend on which view toggles
// happened to be on, which is an untraceable data corruption.

#include <string>
#include <vector>

#include "elanora/theme.hpp"

// Included rather than forward-declared: ImVec2 appears in default arguments
// below, which needs the complete type.
#include "imgui.h"

namespace elanora {

// ---------------------------------------------------------------------------
// Display chain
// ---------------------------------------------------------------------------

struct DisplaySettings {
    // Fixed sensitivity, never auto-fit. Auto-scaling amplifies whatever noise
    // is present until it fills the lane, which makes a dead electrode look
    // identical to a healthy one. A quiet channel must look quiet.
    double uv_per_div = 100.0;

    // Raw Muse EEG carries a large DC offset and slow electrode drift; without
    // the high-pass the trace walks off-screen within seconds.
    bool highpass = true;
    double highpass_hz = 0.5;

    // Mains hum is often the largest single component in a raw trace and
    // visually swamps real EEG.
    bool notch = true;
    double notch_hz = 60.0;
};

// One-pole high-pass. NaN (a dropped packet) passes through untouched and
// resets the filter state, so a gap cannot ring into the samples after it.
void apply_highpass(std::vector<double>& v, int sampling_rate, double cutoff_hz);

// Biquad notch. Same NaN handling as above.
void apply_notch(std::vector<double>& v, int sampling_rate, double f0, double q);

// Runs the enabled stages in order and returns a fresh copy. The input is
// never modified, because the caller's buffer is the raw record.
std::vector<double> display_chain(const std::vector<double>& raw,
                                  int sampling_rate,
                                  const DisplaySettings& settings);

// ---------------------------------------------------------------------------
// Trace rendering
// ---------------------------------------------------------------------------

// Draws one channel with min/max decimation into the current window's draw
// list, inside the rectangle (origin, size).
//
// For each pixel column the renderer draws a vertical span from the minimum to
// the maximum of every sample landing in it. The naive alternative -- take
// every Nth sample -- ALIASES: 2048 samples into 800 px turns a 40 Hz gamma
// burst into a slow wobble that is not in the signal. Min/max invents nothing
// and hides nothing, and it is what real acquisition software does.
//
// A NaN sample marks a dropped packet and BREAKS the polyline. Never bridge
// (a straight line across missing data looks exactly like real signal) and
// never substitute zero (that draws a hard spike to baseline which reads as a
// physiological event).
//
// Returns the number of samples that were NaN, so callers can report the loss.
int draw_trace_minmax(const std::vector<double>& samples,
                      ImVec2 origin, ImVec2 size,
                      double units_full_scale,
                      theme::Rgba color,
                      bool glow = true);

// Vertical time gridlines with second labels along the bottom. Without a time
// reference a trace is a picture; with one it is a measurement -- you can say
// how long a burst lasted rather than only that it happened.
void draw_time_grid(ImVec2 origin, ImVec2 size, double span_seconds,
                    double div_seconds = 1.0);

// Calibration bar: a vertical rule one division tall, labelled in microvolts.
// Every clinical EEG display carries one, because "100 uV/div" in a caption is
// far harder to use than a mark you can hold a feature against.
void draw_scale_bar(ImVec2 origin, float lane_height, double uv_per_div,
                    theme::Rgba color);

// Power spectral density with the five band regions shaded behind the curve,
// so the band a peak belongs to is readable without consulting a legend.
// `mags` is linear magnitude; it is drawn in dB because the 1/f slope of real
// EEG is a straight line in log and a featureless cliff in linear.
void draw_spectrum(const double* mags, int n_bins, double bin_hz,
                   ImVec2 origin, ImVec2 size, double f_max,
                   double* peak_hz_out);

// One cell of the sensor x band matrix. Fill opacity encodes magnitude, so the
// grid reads as a heatmap at a glance; the dominant cell in a row gets a
// border so dominance survives being read in greyscale.
void band_cell(ImVec2 pos, ImVec2 size, double value, theme::Rgba color,
               bool dominant);

// ---------------------------------------------------------------------------
// Card chrome
//
// ImGui has no card primitive, so these wrap BeginChild with the panel fill,
// border and top-edge gradient the design system specifies.
// ---------------------------------------------------------------------------

// Begin a card. Always pair with end_card(), including when this returns false.
bool begin_card(const char* id, ImVec2 size = ImVec2(0, 0));
void end_card();

// Uppercase, letter-spaced section label.
void eyebrow(const char* text);

// Large tabular readout with a unit suffix, e.g. "72" + "BPM".
void readout(const char* value, const char* unit, theme::Rgba color = theme::kText);

// Status pill with a leading dot. Used for connection state and quality.
void status_pill(const char* text, theme::Rgba color);

// Right-aligns the remainder of the current line.
void right_align(float width);

void text_colored(theme::Rgba c, const char* fmt, ...);
void text_mono(theme::Rgba c, const char* fmt, ...);

}  // namespace elanora
