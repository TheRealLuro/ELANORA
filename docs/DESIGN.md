# Design

The protocol, and the decisions behind it that are easy to get wrong.

---

## The protocol

18 rounds × 120 s ≈ 36 min. Each round is 30 s baseline / 30 s stimulus /
30 s post / 30 s rest.

- **14 stimulus frequencies**, half-octave steps from 0.5 to 45 Hz
- **2 jitter controls** — same pulse count, randomised intervals; isolates *rhythmicity*
- **2 tone controls** — unmodulated carrier; isolates *sound itself*

Controls are never first, never last, and never adjacent. Clustered controls
would confound the control condition with time-in-session, which is the thing
they exist to rule out.

### Why 0.5–45 Hz

Three limits bind before the speaker does:

- **Measurement ceiling.** Muse 2 EEG is usable to ~50 Hz, and 60 Hz mains sits
  just above. Gamma is capped at 45, not 50, to stay clear of the filter skirt.
- **Window floor.** A 30 s stimulus needs ~10 cycles to establish entrainment,
  putting the floor near 0.35 Hz.
- **Perceptual fusion.** Above ~30–40 Hz a pulse train is heard as timbre
  rather than rhythm.

Frequencies above 45 Hz would produce responses this headset physically cannot
record.

### Three envelope shapes

| | Sounds like | Why it exists |
|---|---|---|
| **Rhythmic** | `WOO_WOO_WOO` | Hard on/off. The envelope is a square wave, so a 10 Hz round also drives 30 and 50 Hz |
| **Wave** | `WOOoo_WOOoo` | Sinusoidal. Energy at the rate and nowhere else — the only shape that can attribute a response to the rate itself |
| **Swell** | `WOOOOOOOOOO` | Continuous at 35% depth. Never silent, so there is no onset to startle at |

One shape per session. Interleaving both would double a session to 72 minutes,
and the design already asks for ≥2 sessions per subject — so running one shape
in each gives the within-subject comparison for free. Alternate which comes
first between subjects, or shape confounds with session order.

### Equal loudness

All conditions are RMS-matched within 5%, and every stimulus rate is equally
loud within 1%.

The second number is not decoration. The filter that rounds gate edges removes
more energy the more edges there are, so before correction the **45 Hz round
was 17% quieter than the 0.5 Hz one** — loudness covarying with the independent
variable, which is exactly the confound RMS matching exists to eliminate.

It would not have been caught by the three-condition check, which compares stim
against control at *one* rate rather than rates against each other.

### Finer coverage without longer sessions

Quarter-octave spacing is 27 frequencies — 62 minutes in one sitting, and a
fatigued subject in round 28 is worse than no data.

So session *b* of *n* takes every *n*th frequency. Two sessions give
quarter-octave, four give eighth, each staying near 36 minutes. **Interleaved
rather than split in half**: every session still spans 0.5–45 Hz, so a
session-level shift — placement, sleep, time of day — lands across the whole
range instead of concentrating on one end and masquerading as a frequency
effect.

---

## Analysis decisions

**Grouped validation only.** Leave-one-session-out and leave-one-subject-out.
Random row splits leak, because trials from one session share electrode
placement and baseline state, and would inflate R² into meaninglessness.

**Absolute log-power is the modeling target.** The five relative band powers
sum to 1, so their deltas sum to zero *by construction* — "alpha up, beta down"
can be pure normalisation artifact rather than physiology. Both representations
are stored; CLR fixes the geometry of relative power (unbounded, ratio-additive)
but does **not** remove the linear dependency. Only absolute power escapes the
constraint.

**22 outcomes, so p-values are FDR-corrected.** At α=0.05 you expect one false
positive by chance, which would let the gate pass on noise — worse than having
no gate at all.

**RMSSD, not SDNN.** SDNN conventionally needs ≥60 s and at 30 s is not
comparable to any published figure. RMSSD is the accepted ultra-short-window
metric. ΔBPM remains the primary heart target; RMSSD stays exploratory.

**Quality is per-signal, never per-trial.** A trial with clean EEG and unusable
breathing feeds the four brain models and is excluded only from the breathing
model.

**Muscle activity is a flag, not a sensor.** The Muse 2 has no EMG, so jaw
clench and movement are inferred from EEG excursions plus IMU motion energy
band-passed 1–10 Hz — deliberately above the respiratory band so normal
breathing cannot trip it — and used to discount suspect gamma.

**Language is associational, never causal.** The UI says "associated with a
predicted increase," never "causes." Control comparison and repetition
strengthen the inference; they do not license the word.

---

## Data integrity

**Raw data is never overwritten.** Feature and ML CSVs are always regenerable
from `raw/`.

**Display DSP never reaches a CSV.** Every stored feature is recomputed from
raw samples, so numbers cannot depend on which device ran the session or which
view toggles happened to be on.

**Timestamps are reconstructed.** The Muse sends a packet counter and no clock,
so time comes from sequence differences anchored to the audio clock. This is a
permanent weakness of the phone path relative to a BrainFlow board timestamp,
and it is why dropped-packet counts are reported rather than hidden.

**Gaps stay gaps.** A dropped packet is stored as an empty field and rendered
as a break. Bridging it looks like real signal; zero-filling it draws a spike
that reads as a physiological event.

**A round is saved only after the full cycle.** Baseline, stimulus, post, and
the questions. Stopping early keeps every completed round and discards the one
in progress — a round stored without its survey would be missing the
manipulation check, and nothing downstream could tell.

**Every trial carries a condition label.** There is no such thing as an
unlabelled trial; the whole design rests on comparing stimulus against control.

---

## Expected results that are not bugs

- **Subject-level R² well below session-level.** Frequency response is highly
  individual. That gap is the real measure of whether this generalises to new
  people.
- **Breathing confidence below 0.5**, excluded from training. Expected even at
  30 s windows.
- **Gamma most contaminated by jaw tension.** Always cross-read against
  `motion_energy` and `eeg_excursion` before believing a gamma finding.
- **The optimizer refusing to recommend.** This is the design working, not
  failing.
- **Every outcome reading `NoEvidence`.** A valid result: frequency did not
  measurably affect this subject pool under this protocol. The honest next step
  is a protocol change, not an optimizer.
