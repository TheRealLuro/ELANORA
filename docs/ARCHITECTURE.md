# Architecture

Inputs and outputs at every stage, in pseudocode, so the pipeline can be
extended or optimised without reading all of it.

Types are written loosely: `f64[]` is an array of doubles, `µV` marks units,
`NaN` marks a deliberate gap.

---

## The pipeline

```
BLE packets ──> samples ──> timestamped series ──> CSV ──> features ──> evidence ──> models
   muse.js     decode.js     ringbuffer.js       upload.js  data/       data/        models/
                                                 server/
```

Each arrow is a contract. Nothing downstream reads anything but the output of
the stage above it.

---

## 1. Acquisition — `web/muse.js`

```
Muse.connect()
  in:   (user gesture)
  out:  resolves once GATT is open and all four EEG characteristics exist
  fail: throws with the reason — not a secure context, no Web Bluetooth,
        missing characteristic

Muse.start()
  in:   -
  out:  begins notifications; writes "p50" (enable PPG) then "d" (stream)
  note: p50 must precede d — a preset change mid-stream is ignored silently

Muse.onRaw(cb)
  cb:   (name: "TP9"|"AF7"|"AF8"|"TP10"|"ppgIr"|…|"telemetry", dv: DataView)
        dv is one 20-byte packet, always
```

## 2. Decode — `web/decode.js`

Pure functions. No state, no clock, no allocation beyond the result.

```
decodeEeg(dv)  -> { seq: u16, samples: f64[12] }   // µV
decodePpg(dv)  -> { seq: u16, samples: f64[6]  }   // raw counts
decodeImu(dv, scale) -> { seq: u16, samples: [x,y,z][3] }
decodeTelemetry(dv)  -> { seq, batteryPct, fuelGaugeUv, adcMv, temperatureC }

unwrap(prev: u32, seq: u16) -> u32
  Extends a 16-bit counter monotonically. At 256 Hz it wraps every ~51 min,
  inside a single session, so this is load-bearing rather than defensive.
```

EEG packing is 12 samples × 12 bits in 18 bytes, two samples per three bytes.
`µV = (raw - 0x800) × 125/256`.

## 3. Timebase — `web/ringbuffer.js`

The headset sends **no clock**, only a packet counter. This stage invents time
from sequence differences.

```
Stream(rate_hz, values_per_packet, seconds = 240)

  anchor(t0)                 // perf-time of the first sample
  push(seq, values[])        // fills NaN for every missing packet
  slice(t0, t1) -> { t: f64[], v: f64[] }     // [t0, t1)
  .dropped -> count of NaN samples inserted

  timeAt(i) = t0 + (origin + i) / rate
```

Three rules that are easy to get wrong:

- Time is measured from the **first packet received**, never from sequence zero.
  The counter is free-running; anchoring on its absolute value puts timestamps
  ~23 minutes in the future.
- A stream of interleaved axes counts **values, not readings**. IMU is
  `Stream(52 × 3, 3 × 3)`, not `Stream(52, 9)`.
- Gaps stay `NaN`. Never bridge (looks like signal), never zero (looks like an
  event).

## 4. Stimulus — `web/tone.js` / `collector/src/tone.cpp`

```
renderStimulus(ctx, {
  condition:    "stim" | "control_jitter" | "control_tone",
  envelope:     "gated" | "wave" | "swell",
  hz, jitterMeanHz, seed, carrierHz, duty, seconds, amplitude
}) -> AudioBuffer
```

The whole round is rendered ahead of time and started with `source.start(t)`,
which is sample-accurate. Scheduling gate edges live would put tens of
milliseconds of jitter on the one timing the experiment depends on.

**These two implementations must stay numerically identical.** `tools/tone_reference`
emits values from the C++; the JavaScript suite asserts against them to 1e-6.
After any change to `tone.cpp`:

```bash
tone_reference > web/fixtures/tone_reference.json
```

## 5. Protocol — `web/protocol.js`

```
Round({ baseline, stimulus, post, rest })
  start(t0)
  phaseAt(t)   -> "baseline"|"stimulus"|"post"|"rest"|"done"   // emits markers
  stimulusAt   -> absolute time the stimulus buffer must start
  markers()    -> [{ ts, event }]   exactly four, in order
```

The caller passes the time in. Markers are stamped **at the boundary**, not at
the polling instant, so a slow frame cannot move one.

## 6. Storage — `web/upload.js` → `server/src/round_store.cpp`

```
toCsv(header: string[], rows: any[][]) -> string
  numbers at 6 dp; NaN becomes an EMPTY FIELD, never the text "NaN"

envelope(meta: {k:v}, sections: {name: csv}) -> string
  "k: v" header lines, blank line, then "@name <bytes>\n<bytes>"
```

Wire format is length-prefixed rather than JSON: a round is ~400 KB of commas
and newlines, and an escaping bug would corrupt samples in ways that still
parse as valid CSV.

```
parse_round(body, &out, &err) -> bool
store_round(root, r, &err)    -> bool
  validates BEFORE writing:
    header matches schema.hpp      else reject
    session_id has no path escape  else reject
    condition is one of three      else reject
    trial_id not already stored    else reject
  writes raw/<session>/<trial>_{eeg,ppg,imu,markers}.csv + a trials.csv row
```

## 7. Features — `data/`

```
band_powers(samples: f64[], sr) -> { abs: f64[5], rel: f64[5], total }
heart_from_ppg(ppg_ir, sr)      -> { bpm, rmssd, n_beats, quality }
breath_from_imu(axes[6], sr)    -> { breaths_per_minute, bpm_spectral,
                                     n_cycles, axis_used, confidence }
assess_period(eeg, ppg, imu, …) -> { eeg[4], heart, breath, motion_energy, … }

build_features(root)     // raw CSVs   -> features/*.csv   (long)
build_ml_datasets(root)  // features   -> ml/*.csv          (wide, per trial)
```

Responses are stored three ways per representation: `d_` (stimulus − baseline),
`post_d_` (post − baseline), `recovery_` (post − stimulus).

Absolute power is modelled as **log-ratio**, so a doubling is +0.693 regardless
of starting level.

## 8. Evidence — `data/evidence.hpp`

The gate. Everything downstream is meaningless without it.

```
analyze(datasets) -> OutcomeEvidence[22]
  per outcome:
    effect_stim_vs_control  Welch t against pooled controls
    p_raw, p_fdr            Benjamini-Hochberg across ALL 22
    cv_r2, cv_r2_null_mean  grouped CV against a label-shuffled null
    verdict                 Supported | Weak | NoEvidence

  Supported  requires p_fdr < 0.05 AND cv_r2 > cv_r2_null_mean
```

22 outcomes at α=0.05 gives ~1 false positive by chance. Without correction the
gate would reliably pass on noise, which is worse than having no gate.

## 9. Models — `models/`

```
ModelRegistry.train_all(datasets)
  brain[S1..S4]: [log2_freq, baseline_logabs×5] -> d_logabs×5
  heart:         [log2_freq, baseline_bpm]      -> d_bpm
  breathing:     [log2_freq, baseline_breathing]-> d_breathing

  each output picks its own model via the ladder:
    MeanBaseline | Ridge | PolyRidge | Gp
    complex wins only by >0.02 R², else the simpler model takes it

predict(registry, frequency_hz, baseline) -> Prediction { 22 means, 22 sds }

optimize(registry, evidence, desired, baseline) -> { found, reason, ranked[] }
  sweeps 200 points in log2 space across the TRAINED range only
  score = goal_match − uncertainty_cost − extrapolation_cost
  refuses when: any goal targets a NoEvidence outcome
                best score < min_score
                best effect < 2× control-trial SD
```

Models consume `log2(frequency)`, never raw Hz. The design is log-spaced; a
linear feature would let 32 and 45 Hz dominate and effectively discard
everything below 4 Hz.

---

## Optimisation notes

Measured hot paths, in the order worth looking at.

| Where | Cost | Called | Notes |
|---|---|---|---|
| `dsp.js` `bandPowers` | O(n log n), nfft 256 | 4×/sec × 4 sensors | Welch over a 2 s window. The single biggest cost on the phone. |
| `dsp.js` `bandpass4` | O(n) × 4 sections | 4×/sec | Allocates a new array per section — four per call. Fusing them into one pass is the obvious win. |
| `tone.js` `renderStimulus` | O(seconds × sr) | once per round | 30 s × 48 kHz ≈ 1.4 M iterations, off the audio thread. Fine, but it blocks the main thread for ~50 ms. |
| `ringbuffer.js` `slice` | O(buffer) | 4×/sec | Linear scan of up to 240 s. A binary search on the timebase would make it O(log n); it has not mattered yet. |
| `draw_trace_minmax` | O(samples) | per frame | Min/max decimation, already the cheap option. Never point-skip: it aliases. |
| `evidence.cpp` permutation | 5000 refits × 22 | once per rebuild | The slowest thing in the project by far. Embarrassingly parallel. |

Two things deliberately **not** optimised:

- **Display DSP runs at 4 Hz, not per frame.** A 256-point Welch per sensor at
  60 fps is pure waste, and the smoothing already reads as continuous.
- **The stimulus buffer is not streamed.** Rendering it whole costs memory and
  a one-off pause; streaming it would cost timing accuracy, which is not a
  trade this experiment can make.

---

## Extending it

| To add… | Touch |
|---|---|
| A new envelope shape | `Envelope` in `session.hpp`, the branch in `tone.cpp`, the same branch in `tone.js`, regenerate the fixture, add a UI button |
| A new stimulus condition | `Condition` in `types.hpp`, `build_schedule`, the tone generator, and the server's condition whitelist |
| A new physiological feature | a `*_features.{hpp,cpp}` pair in `data/`, its columns in `dataset_builder`, then an outcome in `evidence.cpp` |
| A new model type | `ModelKind` in `model_ladder.hpp` and a fit/predict pair; the ladder picks it up automatically |
| A new dataset column | `schema.hpp` only — both writers read from there |
| Another device | a sibling of `device/`, producing the same `Sample` stream |

**Anything that changes the stimulus must keep the two generators in step.**
The conformance test is the guard, and it has caught silent divergence twice.
