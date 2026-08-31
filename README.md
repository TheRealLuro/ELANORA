# ELANORA

Muse 2 physiological frequency response system: learn how audio stimulus
frequencies change brain, heart and breathing activity (the **forward
problem**), then invert it to find the frequency most likely to produce a
desired physiological state (the **inverse problem**).

The design principle that governs everything else: **the system must be able to
show that frequency does nothing.** Statistical evidence gates the optimizer
rather than decorating it, and the optimizer is allowed to answer "no suitable
frequency found."

Full design and task breakdown: `C:\Users\Jdog1\.claude\plans\woolly-sleeping-moon.md`

## Layout

| Folder | Role |
|---|---|
| `common/` | Shared types, CSV layer, UI shell and theme |
| `LSL/` | Muse 2 device I/O and streaming, plus `muse_monitor` diagnostics |
| `collector/` | Stimulus generation and the trial runner app |
| `data/` | Feature extraction, quality control, statistics, dataset builder |
| `models/` | Ridge/GP models, validation, forward prediction, inverse optimizer |

Dependency order is `common <- LSL <- collector` and `common <- data <- models`.

## Prerequisites

A C++20 toolchain and CMake. On Windows:

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

## Building

Quick toolchain check — seconds, fetches only Catch2:

```bash
cmake -B build -S . -DELANORA_BUILD_APPS=OFF -DELANORA_WITH_BRAINFLOW=OFF -DELANORA_WITH_EIGEN=OFF
cmake --build build
ctest --test-dir build --output-on-failure
```

Full build, once the milestones needing them land:

```bash
cmake -B build -S .
cmake --build build --config Release
```

Binaries land in `build/bin/`.

## Status

Milestone 0 (foundation) is scaffolded: build system, shared types, CSV layer
and their tests. **Nothing has been compiled or run yet** — no C++ toolchain was
present on the development machine when this was written, so every test below
is unverified and should be treated as a hypothesis until `ctest` passes.

| Milestone | State |
|---|---|
| 0 Foundation | Written, unverified |
| 1 Device layer | Not started |
| 2 Collector | Not started |
| 3 Features | Not started |
| 4 Evidence gate | Not started |
| 5 Models | Not started |
| 6 Optimizer | Not started |
