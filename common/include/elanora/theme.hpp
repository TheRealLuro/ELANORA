#pragma once
//
// One visual identity for all four ELANORA applications.
//
// The apps are separate executables, but to the person running an experiment
// they are one system. Sharing the palette, metrics and fonts is what makes
// them read that way rather than as four unrelated tools.
//
// Palette follows the "Financial Dashboard (dark)" profile; layout metrics
// follow the "data-dense dashboard" convention -- tight padding, grid layout,
// maximum data visibility. Saturation is reserved for signal and status: the
// accent is deliberately desaturated so the data is the brightest thing on
// screen during a long session in a dim room.

struct ImFont;

namespace elanora {

// Applies colours, rounding and spacing to the current ImGui context.
// Call once after the context exists, before the first frame.
void apply_elanora_theme();

// ---------------------------------------------------------------------------
// Fonts
//
// ImGui's built-in face is ProggyClean, a 13px bitmap font from 2005. It is
// the single largest reason an ImGui program reads as a debug overlay instead
// of an application. load_fonts() builds a four-size ramp, trying bundled Fira
// first, then system Segoe UI / Consolas, then giving up gracefully.
// ---------------------------------------------------------------------------
struct Fonts {
    ImFont* body    = nullptr;  // 15px  -- default UI text
    ImFont* subhead = nullptr;  // 17px  -- card titles
    ImFont* display = nullptr;  // 38px  -- primary readouts
    ImFont* mono    = nullptr;  // 13px  -- data, timestamps, channel names
    ImFont* eyebrow = nullptr;  // 11px  -- uppercase section labels
    bool    bundled = false;    // true when Fira was found rather than fallback
};

// Must be called before the first frame and after the ImGui context exists.
Fonts& load_fonts();
const Fonts& fonts();

namespace theme {

struct Rgba { float r, g, b, a; };

// Surfaces
inline constexpr Rgba kGround  {0.008f, 0.024f, 0.090f, 1.0f};  // #020617
inline constexpr Rgba kPanel   {0.043f, 0.067f, 0.125f, 1.0f};  // #0B1120
inline constexpr Rgba kPanelHi {0.063f, 0.102f, 0.180f, 1.0f};  // #101A2E
inline constexpr Rgba kRaised  {0.075f, 0.106f, 0.180f, 1.0f};  // #131B2E
inline constexpr Rgba kLine    {0.118f, 0.161f, 0.231f, 1.0f};  // #1E293B
inline constexpr Rgba kLineHi  {0.200f, 0.255f, 0.333f, 1.0f};  // #334155

// Text
inline constexpr Rgba kText    {0.973f, 0.980f, 0.988f, 1.0f};  // #F8FAFC
inline constexpr Rgba kDim     {0.796f, 0.835f, 0.882f, 1.0f};  // #CBD5E1
inline constexpr Rgba kMuted   {0.580f, 0.639f, 0.722f, 1.0f};  // #94A3B8
inline constexpr Rgba kFaint   {0.392f, 0.455f, 0.545f, 1.0f};  // #64748B

// Desaturated on purpose. Saturation belongs to data, not to chrome.
inline constexpr Rgba kAccent  {0.220f, 0.741f, 0.973f, 1.0f};  // #38BDF8

// Semantic state, kept separate from the accent hue so "good" never reads as
// "primary action".
inline constexpr Rgba kGood    {0.133f, 0.773f, 0.369f, 1.0f};  // #22C55E
inline constexpr Rgba kWarn    {0.961f, 0.620f, 0.043f, 1.0f};  // #F59E0B
inline constexpr Rgba kBad     {0.937f, 0.267f, 0.267f, 1.0f};  // #EF4444

// The only fully saturated hues on screen: signal. Fixed per band so a band is
// the same colour in every chart in every app.
inline constexpr Rgba kDelta {0.655f, 0.545f, 0.980f, 1.0f};  // #A78BFA
inline constexpr Rgba kTheta {0.376f, 0.647f, 0.980f, 1.0f};  // #60A5FA
inline constexpr Rgba kAlpha {0.176f, 0.831f, 0.749f, 1.0f};  // #2DD4BF
inline constexpr Rgba kBeta  {0.984f, 0.749f, 0.141f, 1.0f};  // #FBBF24
inline constexpr Rgba kGamma {0.957f, 0.447f, 0.714f, 1.0f};  // #F472B6

// Spacing scale, density 8/10 (dense dashboard).
inline constexpr float kS1 = 4.0f;
inline constexpr float kS2 = 8.0f;
inline constexpr float kS3 = 12.0f;
inline constexpr float kS4 = 16.0f;
inline constexpr float kS5 = 24.0f;

inline constexpr float kRadius   = 8.0f;
inline constexpr float kRadiusSm = 5.0f;

// Per-band colour by Band index, so callers do not repeat the mapping.
const Rgba& band_color(int band_index);

}  // namespace theme
}  // namespace elanora
