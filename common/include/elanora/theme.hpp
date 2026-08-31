#pragma once
//
// One visual identity for all four ELANORA applications.
//
// The apps are separate executables, but to the person running an experiment
// they are one system. Sharing the palette and metrics is what makes them read
// that way rather than as four unrelated tools.

namespace elanora {

// Applies colours, rounding and spacing to the current ImGui context.
// Call once after the context exists, before the first frame.
void apply_elanora_theme();

// Semantic colours, exposed so status indicators mean the same thing in every
// app: a red quality lamp in the monitor is the same red as a failed model
// card in the trainer.
namespace theme {

struct Rgba { float r, g, b, a; };

inline constexpr Rgba kGood    {0.30f, 0.78f, 0.47f, 1.0f};  // signal usable
inline constexpr Rgba kFair    {0.90f, 0.71f, 0.25f, 1.0f};  // usable, degraded
inline constexpr Rgba kBad     {0.88f, 0.34f, 0.34f, 1.0f};  // unusable
inline constexpr Rgba kAccent  {0.36f, 0.60f, 0.94f, 1.0f};  // primary action
inline constexpr Rgba kMuted   {0.55f, 0.58f, 0.64f, 1.0f};  // secondary text

// Per-band plot colours. Fixed so a band is the same colour in every chart in
// every app -- alpha is always this blue, whether in the monitor or the model
// trainer.
inline constexpr Rgba kDelta {0.55f, 0.42f, 0.78f, 1.0f};
inline constexpr Rgba kTheta {0.32f, 0.55f, 0.85f, 1.0f};
inline constexpr Rgba kAlpha {0.28f, 0.75f, 0.72f, 1.0f};
inline constexpr Rgba kBeta  {0.92f, 0.62f, 0.30f, 1.0f};
inline constexpr Rgba kGamma {0.85f, 0.40f, 0.55f, 1.0f};

}  // namespace theme
}  // namespace elanora
