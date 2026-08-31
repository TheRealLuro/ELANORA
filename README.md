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

Milestone 0 (foundation) is **complete and verified**: configured, compiled
warning-free under `/W4`, and all 24 tests pass. The GUI stack is verified by
`ui_smoke`, which opens a real window, renders through ImGui and ImPlot, and
tears down cleanly.

Verified toolchain: MSVC 19.44 (VS 2022 BuildTools), CMake 4.x, Windows SDK
10.0.26100.

| Milestone | State |
|---|---|
| 0 Foundation | **Verified** — build, tests, `ui_smoke` |
| 1 Device layer | Task 4 (MuseDevice) **verified**; Tasks 5-7 not started |
| 2 Collector | Not started |
| 3 Features | Not started |
| 4 Evidence gate | Not started |
| 5 Models | Not started |
| 6 Optimizer | Not started |

## Running the checks

```bash
cmake -B build -S .
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
./build/bin/Debug/ui_smoke --frames 60
./build/bin/Debug/muse_probe          # prints the Muse 2 channel map, no headset needed
```

### Verified hardware facts

`muse_probe` output, confirmed against BrainFlow 5.16.0:

| Preset | Rate | Channels |
|---|---|---|
| DEFAULT (EEG) | 256 Hz | 4 — TP9, AF7, AF8, TP10 |
| AUXILIARY (IMU) | 52 Hz | 3 accel + 3 gyro |
| ANCILLARY (PPG) | 64 Hz | 3 — `[0]` red 660nm, `[1]` IR 940nm, `[2]` ambient |

The datasheet's "2-channel PPG" counts active wavelengths; BrainFlow also
exposes the ambient reference channel.
