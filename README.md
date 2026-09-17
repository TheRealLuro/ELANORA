# ELANORA

**Does audio stimulus frequency change brain, heart or breathing activity — and if so, which frequency produces a desired state?**

A Muse 2 EEG research system that records controlled trials under isochronic audio, extracts physiological features, tests whether frequency predicts anything at all, and only then inverts the models to recommend a stimulus frequency.

![Live EEG monitor](docs/images/monitor-waveforms.png)

---

## The principle this is built around

**The system must be able to show that frequency does nothing.**

An optimizer that always returns a frequency is worthless if frequency turns out not to predict anything. So statistical evidence *gates* the recommendation rather than decorating it, and the optimizer is allowed to answer **"no suitable frequency found."**

Three refusal paths, all of which must pass before a recommendation appears:

| Gate | Refuses when |
|---|---|
| **Evidence** | The outcome shows no statistically supported frequency effect (FDR-corrected across 22 outcomes) |
| **Trained range** | The candidate frequency lies outside what was actually measured |
| **Effect size** | The predicted change is smaller than 2× the noise seen on control trials |

If every outcome reads `NoEvidence`, that is a **valid result** — not a bug to work around.

---

## How it works

```
  iPhone (Bluefy)                        PC
  ┌────────────────────┐          ┌────────────────────┐
  │ Web Bluetooth      │          │ elanora_serve      │
  │ stimulus + protocol│ ─HTTPS─> │ receives rounds    │ ──> data/datasets/
  │ live signal check  │          └────────────────────┘          │
  └─────────┬──────────┘                                          v
            │ BLE                              elanora_data ──> elanora_models
       Muse 2 headset                          features +        train +
                                               evidence gate     recommend
```

The phone runs the session because acquisition needs a Bluetooth radio, and it generates the audio locally — a wifi round trip in the stimulus path would put jitter on the one timing the experiment most depends on. The PC stores the data and does the analysis.

### Verified on real hardware

One recorded round, straight off the headset:

```
markers   exactly 30.000 s apart across baseline / stimulus / post
EEG       23,271 samples · 90.9 s · 256.0 Hz · zero dropped packets
          TP9 33 µV   AF7 13 µV   AF8 20 µV   TP10 36 µV
PPG       64.0 Hz, clear pulse waveform
IMU       52.0 Hz, 0.94-1.02 g
```

---

## Screenshots

**Collector** — 18-round protocol, shuffled order with controls distributed, pre-flight electrode check

![Collector](docs/images/collector-setup.png)

**Trial detail** — raw EEG, PPG and IMU with the three periods shaded from the markers

![Raw signals](docs/images/data-raw-signals.png)

**Band power** — per sensor across baseline, stimulus and post

![Band power](docs/images/data-band-power.png)

**Collection status** — rounds counted from files on disk, not from trial rows

![Status](docs/images/collection-status.png)

---

## Quick start

```bash
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build -C Release     # 260 tests
```

Run a session:

```bash
./build/bin/Release/elanora_serve --port 8080 --web web --root data/datasets
```

It prints a write token and the LAN URL. Open that URL on the phone, connect the headset, check the electrodes, press Start. Rounds upload as each one finishes; watch them land at `/data.html`.

Then analyse:

```bash
./build/bin/Release/elanora_data      # features + the evidence gate
./build/bin/Release/elanora_models    # train, then invert
```

JavaScript tests run in a browser rather than a test runner — open `/tests.html` (90 tests, including a numerical conformance check between the browser and C++ stimulus generators).

Full setup notes, including the iOS requirements: **[docs/SETUP.md](docs/SETUP.md)**

---

## The protocol

18 rounds × 120 s ≈ 36 min. Each round is 30 s baseline / 30 s stimulus / 30 s post / 30 s rest.

- **14 stimulus frequencies**, half-octave steps from 0.5 to 45 Hz
- **2 jitter controls** — same pulse count, randomised intervals; isolates *rhythmicity*
- **2 tone controls** — unmodulated carrier; isolates *sound itself*

**Three envelope shapes**, one per session:

| | Sounds like | Why it exists |
|---|---|---|
| **Rhythmic** | `WOO_WOO_WOO` | Hard on/off. Envelope is a square wave, so 10 Hz also drives 30 and 50 Hz |
| **Wave** | `WOOoo_WOOoo` | Sinusoidal. Energy at the rate and nowhere else — the only one that can attribute a response to the rate itself |
| **Swell** | `WOOOOOOOOOO` | Continuous at 35% depth. Never silent, so there is no onset to startle at |

All conditions are RMS-matched within 5%, and every stimulus rate is equally loud within 1%.

That second number is not decoration. The filter that rounds gate edges removes more energy the more edges there are, so before it was corrected the **45 Hz round was 17% quieter than the 0.5 Hz one** — loudness covarying with the independent variable, which is exactly the confound the RMS matching exists to eliminate.

### Finer coverage without longer sessions

Quarter-octave spacing is 27 frequencies — 62 minutes in one sitting, and a fatigued subject in round 28 is worse than no data. So session *b* of *n* takes every *n*th frequency: two sessions give quarter-octave, four give eighth, each staying near 36 minutes. Interleaved rather than split in half, so every session still spans 0.5-45 Hz and a session-level shift cannot masquerade as a frequency effect.

---

## Design decisions worth knowing

- **Grouped validation only.** Leave-one-session-out and leave-one-subject-out. Random row splits leak, because trials from one session share electrode placement and baseline state.
- **Absolute log-power is the modeling target.** Relative band powers sum to 1, so their deltas sum to zero by construction — "alpha up, beta down" can be pure normalisation artifact. Both are stored; CLR fixes the geometry of relative power but not the dependency.
- **22 outcomes, so p-values are FDR-corrected.** At alpha = 0.05 you expect one false positive by chance, which would let the gate pass on noise.
- **Quality is per-signal, never per-trial.** Clean EEG with unusable breathing still trains the four brain models.
- **Timestamps are reconstructed.** The Muse sends a packet counter and no clock, so time comes from sequence differences anchored to the audio clock. Dropped packets stay as NaN gaps — never bridged, never zero-filled.
- **Display DSP never reaches a CSV.** Every stored feature is recomputed from raw samples, so numbers cannot depend on which device ran the session.

### Expected results that are not bugs

- Subject-level R² well below session-level. Frequency response is individual, and that gap is the real measure of whether this generalises.
- Breathing confidence below 0.5 and excluded from training.
- Gamma most contaminated by jaw tension — cross-read against motion energy.
- The optimizer refusing to recommend.

---

## Layout

| Folder | Role |
|---|---|
| `common/` | Shared types, CSV layer, UI shell |
| `device/` | Muse 2 device I/O, signal quality, `muse_monitor` |
| `collector/` | Stimulus generation, trial runner, protocol constants |
| `data/` | Features, quality control, statistics, the evidence gate |
| `models/` | Ridge/GP models, grouped validation, forward and inverse |
| `server/` | `elanora_serve` — hosts the phone app, receives recordings |
| `web/` | The phone collector |
| `tools/` | Developer tools, not part of the shipped system |

Detailed breakdown: **[docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md)**

---

## Built with

C++20 · CMake · BrainFlow · Dear ImGui + ImPlot · Eigen · Catch2 · miniaudio · cpp-httplib · Web Bluetooth · Web Audio

No Python or Node in the shipped system.

## Documentation

- **[docs/SETUP.md](docs/SETUP.md)** — running a session, including iOS specifics
- **[docs/PROJECT_STRUCTURE.md](docs/PROJECT_STRUCTURE.md)** — every folder and what lives in it
- **[docs/specs/](docs/specs/)** — design decisions and their rationale
- **[docs/plans/](docs/plans/)** — implementation breakdown

## License

[MIT](LICENSE)
