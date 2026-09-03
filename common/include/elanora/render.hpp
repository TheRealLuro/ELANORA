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
// Motion
//
// Nothing in this interface snaps. A number that jumps between frames forces
// the eye to re-read it; a number that eases carries its own history, so you
// can see a value rising without watching it continuously.
//
// Frame-rate independent: the same visual settling time whether the app is
// running at 60 or 144 Hz.
// ---------------------------------------------------------------------------

// tau is the time constant in seconds -- roughly, how long to cover 63% of the
// remaining distance. 0.10-0.15 reads as immediate but not abrupt.
double smooth_to(double current, double target, float dt, double tau = 0.12);

// A value that eases toward whatever it is assigned.
struct Smoothed {
    double value = 0.0;
    double tau   = 0.12;
    void set(double target, float dt) { value = smooth_to(value, target, dt, tau); }
    void snap(double v) { value = v; }
    float f() const { return static_cast<float>(value); }
};

// Colour that eases between states, so a quality change reads as a transition
// rather than a flicker.
theme::Rgba lerp_color(theme::Rgba a, theme::Rgba b, float t);

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

// Segmented control with a selection indicator that slides between segments.
// Replaces a row of loose buttons: the segments are visibly one control with
// one answer, and the slide shows which way the selection moved.
// Returns true on the frame the selection changes.
bool segmented(const char* id, const char* const* labels, int count,
               int* current, float width = 0.0f);

// Animated toggle. A checkbox states a boolean; a switch shows it changing,
// which matters for filters whose effect on the trace is immediate.
bool toggle_switch(const char* id, bool* value);

// ---------------------------------------------------------------------------
// Depth
//
// ImGui has no shadow primitive, so one is built from concentric rounded rects
// with falling alpha. Without it every panel is a flat fill a shade off the
// page, which is the single biggest reason a dark UI reads as unfinished --
// nothing occupies a plane, so nothing has weight.
// ---------------------------------------------------------------------------
void drop_shadow(ImVec2 p0, ImVec2 p1, float rounding,
                 float spread = 22.0f, int layers = 26);

// Ring gauge. A value from 0..1 swept clockwise from twelve o'clock, with
// rounded ends and a recessed track behind it. Large, round and unmistakable
// at a glance -- a number in a box is precise but has no shape to recognise.
void ring_gauge(ImVec2 center, float radius, float thickness, double value,
                theme::Rgba color, theme::Rgba track);

// Chunky rounded bar with a two-stop gradient along its length.
// Replaces a 3px hairline: a bar you can see is a bar you can compare.
void value_bar(ImVec2 pos, ImVec2 size, double value, theme::Rgba color,
               bool emphasised);

// A bar that grows left or right from a centre line, with an uncertainty
// whisker laid over it.
//
// This is the right encoding for a signed change and a bare number is the
// wrong one: direction becomes position, magnitude becomes length, and the two
// read together at a glance instead of requiring the eye to parse a sign.
//
// `uncertainty` draws a whisker in the same units as `value`. When the whisker
// crosses zero the model cannot tell which direction the change went, and the
// bar is drawn darkened to say so -- de-emphasis is done by darkening the
// colour, never by lowering alpha, which would make the bar look see-through
// over the card behind it.
void diverging_bar(ImVec2 pos, ImVec2 size, double value, double full_scale,
                   double uncertainty, theme::Rgba color);

// An XY curve with an optional shaded x-band and a marked x position.
//
// `shade_lo`/`shade_hi` shade a region of the x axis (the trained frequency
// range); pass equal values for no shading. `mark_x` drops a vertical rule at
// the chosen point. Returns the y value at `mark_x` for callers that want to
// label it.
double score_curve(ImVec2 origin, ImVec2 size,
                   const std::vector<double>& xs, const std::vector<double>& ys,
                   double shade_lo, double shade_hi, double mark_x,
                   theme::Rgba color);

// Draws text centred on a point, in the current font. Centring by hand at
// every call site is where alignment drifts.
void text_centered(ImVec2 center, const char* text, theme::Rgba color);

// Large soft radial glow, built from concentric circles with falling alpha.
//
// Glassmorphism needs something behind the glass. Translucency over a flat
// colour shows nothing at all -- the panel just looks slightly darker. These
// go down first, and the frosted panels pick up their colour variation.
void soft_glow(ImVec2 center, float radius, theme::Rgba color, float alpha);

// Status glyph: a filled disc carrying a check, a bang, or a cross.
// A symbol is recognised before a word is read, which is what you want when
// the question is "is this electrode on properly" and your hands are busy.
void status_glyph(ImVec2 center, float radius, int level, theme::Rgba color);

// ---------------------------------------------------------------------------
// Card chrome
//
// ImGui has no card primitive, so these wrap BeginChild with the panel fill,
// border and top-edge gradient the design system specifies.
// ---------------------------------------------------------------------------

// Begin a card. Always pair with end_card(), including when this returns false.
// Returns true when the card's contents are visible; draw them only in that
// case. end_card() must be called REGARDLESS of the return value, exactly like
// ImGui::EndChild -- begin_card pushes style state and opens a child window,
// and a culled card that skipped end_card would leave both stacks unbalanced
// and trip an assertion several frames later, far from the cause.
//
//     if (begin_card("id", size)) { ...contents... }
//     end_card();
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
