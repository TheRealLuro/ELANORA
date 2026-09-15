# ELANORA

Does audio stimulus frequency change brain, heart or breathing activity — and
if so, which frequency produces a desired state?

Records controlled Muse 2 EEG trials under isochronic audio, extracts
physiological features, tests whether frequency predicts anything at all, then
inverts the models to recommend a stimulus frequency.

**The governing principle: the system must be able to show that frequency does
nothing.** Statistical evidence gates the optimizer rather than decorating it,
and the optimizer is allowed to answer "no suitable frequency found." An
optimizer that always returns a frequency is worthless if frequency turns out
not to predict anything.

## How it fits together

```
  phone (Bluefy)                     PC
  ┌──────────────┐            ┌──────────────────┐
  │ web collector│ ── wifi ─> │ elanora_serve    │ ─> data/datasets/
  └──────┬───────┘   HTTPS    └──────────────────┘         │
         │ BLE                                             v
      Muse 2                    elanora_data ─> elanora_models
                               features +        train + recommend
                               evidence gate
```

The phone runs the session because acquisition needs a Bluetooth radio. The PC
stores the data and does the analysis.

| Folder | Role |
|---|---|
| `common/` | Shared types, CSV layer, UI shell |
| `device/` | Muse 2 device I/O, `muse_monitor` diagnostics |
| `collector/` | Stimulus generation, trial runner, protocol constants |
| `data/` | Features, quality control, statistics, the evidence gate |
| `models/` | Ridge/GP models, validation, forward and inverse |
| `server/` | `elanora_serve` — hosts the phone app, receives recordings |
| `web/` | The phone collector |
| `tools/` | Developer tools, not part of the shipped system |

## Build and test

```bash
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build -C Release        # 258 tests
```

JavaScript tests run in a browser, not a test runner — start the server below
and open `/tests.html` (73 tests, including a numerical conformance check
between the browser and C++ tone generators).

## Running a session

```bash
./build/bin/Release/elanora_serve --port 8080 --web web --root data/datasets
```

It prints a write token and the LAN URL. Then on the phone:

1. **iPhone needs [Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055)** —
   Safari has no Web Bluetooth and never has. Android Chrome works natively.
2. Open the URL with `?k=<token>` appended. The page stores the token and
   strips it from the address bar.
3. **Auto-Lock → Never.** A screen lock suspends Bluetooth and audio mid-round.
4. Connect the headset, check all four electrodes read `good`, then Start.

Web Bluetooth requires a secure context, so plain `http://` over a LAN may not
work — Bluefy refuses it outright. For HTTPS without installing a certificate
on the phone, tunnel it:

```bash
cloudflared tunnel --url http://localhost:8080
```

Uploads then route through Cloudflare. Fine for testing on yourself; decide
deliberately before collecting from other subjects.

## Analysis

```bash
./build/bin/Release/elanora_data      # build features, run the evidence gate
./build/bin/Release/elanora_models    # train, then invert
```

`elanora_data` has three tabs. **Browse** lists the trials and draws the
selected one's raw EEG, PPG and IMU with the three periods shaded from the
markers — that is how you tell a real alpha rise from a lead that came loose,
since both produce a number. **Features** charts band power per sensor across
the three periods. **Evidence** is the one that matters.

**Read the Evidence tab before trusting any recommendation.** If every outcome
reads `NoEvidence`, that is a valid result: frequency did not measurably affect
this subject pool under this protocol. The honest next step is a protocol
change, not an optimizer.

## The protocol

18 rounds × 120 s ≈ 36 min per session. Each round is 30 s baseline / 30 s
stimulus / 30 s post / 30 s rest.

- **14 stimulus frequencies**, half-octave steps from 0.5 to 45 Hz
- **2 jitter controls** — same pulse count, randomised intervals; isolates
  *rhythmicity*
- **2 tone controls** — unmodulated carrier; isolates *sound itself*

All conditions are RMS-matched within 5%, and every stimulus rate is equally
loud within 1%. That second one is not decoration: the edge-rounding filter
removes more energy the more edges there are, so before it was corrected the
45 Hz round was 17% quieter than the 0.5 Hz one — loudness covarying with the
independent variable.

**Two envelope shapes**, chosen per session:

- **Rhythmic** — hard on/off. The envelope is a square wave, so a 10 Hz round
  also drives 30 and 50 Hz.
- **Wave** — sinusoidal. Energy at the rate and nowhere else.

Only the wave condition can attribute a response to the rate itself, which is
why both exist.

**Finer coverage comes from more sessions, not longer ones.** Quarter-octave
spacing is 27 frequencies — 62 minutes in one sitting, and a fatigued subject
in round 28 is worse than no data. So session *b* of *n* takes every *n*th
frequency: two sessions give quarter-octave, four give eighth, and each stays
near 36 minutes. Interleaved rather than split in half, so every session still
spans 0.5–45 Hz and a session-level shift cannot masquerade as a frequency
effect.

## Constraints worth knowing

- **Grouped validation only.** Leave-one-session-out and leave-one-subject-out.
  Random row splits leak, because trials from one session share electrode
  placement and baseline state.
- **Absolute log-power is the default modeling target.** Relative band powers
  sum to 1, so their deltas sum to zero by construction — "alpha up, beta down"
  can be pure normalisation artifact. Both are stored; CLR fixes the geometry
  of relative power but not the dependency.
- **22 outcomes, so p-values are FDR-corrected.** At α=0.05 you expect one
  false positive by chance, which would let the gate pass on noise.
- **Quality is per-signal, never per-trial.** Clean EEG with unusable breathing
  still trains the four brain models.
- **Phone timestamps are reconstructed.** The Muse sends a sequence number and
  no clock, so time comes from sequence differences anchored to the audio
  clock. Dropped packets stay as NaN gaps, never bridged.

## Expected results that are not bugs

- Subject-level R² well below session-level. Frequency response is individual;
  that gap is the real measure of whether this generalises.
- Breathing confidence below 0.5 and excluded from training.
- Gamma most contaminated by jaw tension — cross-read against motion energy.
- The optimizer refusing to recommend. That is the design working.

## Docs

- `docs/specs/` — design decisions and rationale
- `docs/plans/` — task breakdown
