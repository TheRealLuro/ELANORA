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
// Sizes follow a modular scale (12 / 14 / 18 / 24 / 32 / 44). Arbitrary sizes
// are the thing that makes a layout feel unconsidered even when every element
// is individually fine.
struct Fonts {
    ImFont* eyebrow = nullptr;  // 11  uppercase section labels
    ImFont* body    = nullptr;  // 14  default UI text
    ImFont* subhead = nullptr;  // 18  card titles, secondary readouts
    ImFont* metric  = nullptr;  // 32  card metrics
    ImFont* hero    = nullptr;  // 44  the one number worth looking at first
    ImFont* mono    = nullptr;  // 14  aligned numerics
    ImFont* monoBig = nullptr;  // 24  matrix values
    ImFont* display = nullptr;  // alias of hero, kept for existing callers
    bool    bundled = false;
};

// Must be called before the first frame and after the ImGui context exists.
Fonts& load_fonts();
const Fonts& fonts();

namespace theme {

struct Rgba { float r, g, b, a; };

// Surfaces: NEUTRAL grey, not blue-tinted.
//
// The previous palette carried a blue bias through every surface, every grey
// and the accent, so nothing read as a deliberate colour -- it read as a
// default tech-dashboard template. Neutral greys let a single accent mean
// something; blue greys under a blue accent are mush.
inline constexpr Rgba kGround  {0.043f, 0.043f, 0.047f, 1.0f};  // #0B0B0C
inline constexpr Rgba kPanel   {0.145f, 0.145f, 0.161f, 1.0f};  // #252529
inline constexpr Rgba kPanelHi {0.106f, 0.106f, 0.118f, 1.0f};  // #1B1B1E
inline constexpr Rgba kRaised  {0.192f, 0.192f, 0.212f, 1.0f};  // #313136
inline constexpr Rgba kLine    {0.173f, 0.173f, 0.188f, 1.0f};  // #2C2C30
inline constexpr Rgba kLineHi  {0.310f, 0.310f, 0.341f, 1.0f};  // #4F4F57

// Text: neutral through the whole ramp.
inline constexpr Rgba kText    {0.980f, 0.980f, 0.984f, 1.0f};  // #FAFAFB
inline constexpr Rgba kDim     {0.769f, 0.769f, 0.784f, 1.0f};  // #C4C4C8
inline constexpr Rgba kMuted   {0.541f, 0.541f, 0.565f, 1.0f};  // #8A8A90
inline constexpr Rgba kFaint   {0.361f, 0.361f, 0.388f, 1.0f};  // #5C5C63

// The one saturated colour in the chrome. On neutral greys a single accent
// reads as intentional; it was invisible against blue-grey panels.
inline constexpr Rgba kAccent  {0.376f, 0.647f, 0.980f, 1.0f};  // #60A5FA

// Semantic state, kept separate from the accent hue.
inline constexpr Rgba kGood    {0.133f, 0.773f, 0.369f, 1.0f};  // #22C55E
inline constexpr Rgba kWarn    {0.961f, 0.620f, 0.043f, 1.0f};  // #F59E0B
inline constexpr Rgba kBad     {0.937f, 0.267f, 0.267f, 1.0f};  // #EF4444

// Band hues, used ONLY to label a band -- in a header, a legend, a spectrum
// region. Never to fill a grid of cells: five hues across twenty cells read as
// spreadsheet conditional formatting rather than as data.
inline constexpr Rgba kDelta {0.655f, 0.545f, 0.980f, 1.0f};  // #A78BFA
inline constexpr Rgba kTheta {0.376f, 0.647f, 0.980f, 1.0f};  // #60A5FA
inline constexpr Rgba kAlpha {0.176f, 0.831f, 0.749f, 1.0f};  // #2DD4BF
inline constexpr Rgba kBeta  {0.984f, 0.749f, 0.141f, 1.0f};  // #FBBF24
inline constexpr Rgba kGamma {0.957f, 0.447f, 0.714f, 1.0f};  // #F472B6

// Traces are ONE colour.
//
// Four hues for four EEG channels put four more competing colours on a screen
// that already had nine. Channels are told apart by lane position and label,
// which is how clinical EEG has always done it -- colour adds nothing here and
// costs the whole palette its restraint.
inline constexpr Rgba kTrace    {0.847f, 0.871f, 0.894f, 1.0f};  // #D8DEE4
inline constexpr Rgba kTraceAlt {0.478f, 0.510f, 0.549f, 1.0f};  // #7A828C

// Spacing scale, density 8/10 (dense dashboard).
inline constexpr float kS1 = 4.0f;
inline constexpr float kS2 = 8.0f;
inline constexpr float kS3 = 12.0f;
inline constexpr float kS4 = 16.0f;
inline constexpr float kS5 = 24.0f;
inline constexpr float kS6 = 32.0f;

// Generous, continuous-feeling radii. Tight corners read as utilitarian
// chrome; softer ones let the panel recede and the data come forward.
inline constexpr float kRadius   = 18.0f;
inline constexpr float kRadiusSm = 11.0f;

// Per-band colour by Band index, so callers do not repeat the mapping.
const Rgba& band_color(int band_index);

}  // namespace theme
}  // namespace elanora
