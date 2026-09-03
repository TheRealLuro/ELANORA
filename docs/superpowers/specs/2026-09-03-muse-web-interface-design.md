# ELANORA Web Collector — phone-hosted Muse 2 acquisition

**Status:** approved design, not yet implemented
**Date:** 2026-09-03
**Supersedes nothing.** Extends the system described in `woolly-sleeping-moon.md`.

## Why this exists

The desktop collector cannot record. This machine has no Bluetooth radio —
`Get-PnpDevice -Class Bluetooth` enumerates zero devices, and the board's Intel
AX210 stopped answering USB descriptor requests on 2026-08-30. Native BLE is
therefore unavailable, and the BLED112 dongle path added in `8758a3f` needs
hardware that is not yet purchased.

A phone already has a working BLE radio, speakers, and a screen. Moving
acquisition there is not a convenience feature — **it is currently the only way
the project can collect data at all.**

The governing principle of the parent plan carries over unchanged: the system
must be able to show that frequency does nothing. Nothing in this document
weakens the evidence gate, the control conditions, or the refusal paths.

## Success criteria

1. A full 18-round session runs end to end on the phone without the operator
   touching the PC after pressing start.
2. The CSVs that land in `data/datasets/` are byte-schema-identical to what
   `TrialRecorder` writes, so `elanora_data.exe` reads them with no changes.
3. The three audio conditions are RMS-matched within 5%, verified in the
   browser that will actually play them.
4. A dropped BLE packet becomes a visible gap, never an interpolated value.
5. A suspended tab is detected and the round is marked suspect rather than
   silently recorded as good.

## Hard constraints

| Constraint | Consequence |
|---|---|
| iOS Safari has no Web Bluetooth | Session runs in **Bluefy** (free, App Store). Not a code difference, but a setup instruction that must be impossible to miss. |
| Web Bluetooth requires a secure context | `http://192.168.x.x` may expose no `navigator.bluetooth` at all. Unresolved — see Risk 2. |
| iOS suspends backgrounded tabs | Screen lock kills BLE and audio mid-round. Wake Lock API + `Auto-Lock: Never` + gap detection. |
| Muse sends no wall clock | Timestamps are reconstructed. See "Timestamps" below. |
| One BLE central at a time | The Muse app must be closed. Connection failure should say so. |

## Architecture

```
iPhone — Bluefy                          PC — elanora_serve.exe
┌──────────────────────────┐            ┌────────────────────────────┐
│ muse.js     BLE + decode │            │  serves web/               │
│ dsp.js      quality, PSD │            │  GET  /schedule?seed=...    │
│ tone.js     Web Audio    │ ─ wifi ──> │  POST /round               │
│ protocol.js phases       │            │  POST /survey              │
│ upload.js   CSV + POST   │            │  writes data/datasets/raw/ │
│ app.js      screens      │            └────────────────────────────┘
└──────────────────────────┘                        │
        │ BLE                                       ▼
     Muse 2                                  elanora_data.exe (unchanged)
```

### Why the PC does not drive the protocol

An earlier option had the phone stream EEG to the PC and the PC decide phase
transitions, gating audio over the wire. Rejected: stimulus onset must be tight,
and a wifi round trip puts jitter in the single most timing-sensitive path in
the experiment. A wifi stall would corrupt the stimulus rather than merely
delaying a display update.

### Why the PC still generates the schedule

`GET /schedule` returns the round order as JSON, produced by the existing
`build_schedule()`. The randomisation constraints — no control first or last, no
two controls adjacent, jitter means drawn from the session's frequency set — are
already implemented and tested in C++. Reimplementing them in JavaScript would
create a second source of truth for the experimental design, and the two would
drift. The phone receives a plan and executes it.

The phone caches the returned schedule, so a wifi drop mid-session does not
abort the run; uploads retry.

## Components

### `web/muse.js` — BLE transport and packet decode

```
connect()            -> requestDevice({filters:[{services:[MUSE_SERVICE]}]})
                        then GATT connect, resolve characteristics
start()              -> write "p50" (enable PPG), then "d" (start streaming)
stop()               -> write "h"
on(stream, cb)       -> "eeg" | "ppg" | "imu" | "telemetry"
raw()                -> last N packets, for the inspector screen
```

**GATT identifiers — TO BE VERIFIED against the headset before anything depends
on them.** These are recalled, not read off a datasheet:

| Purpose | UUID |
|---|---|
| Service | `0000fe8d-0000-1000-8000-00805f9b34fb` |
| Control | `273e0001-4c4d-454d-96be-f03bac821358` |
| EEG TP9 / AF7 / AF8 / TP10 | `273e0003` … `273e0006` (same suffix) |
| Gyroscope / Accelerometer | `273e0009` / `273e000a` |
| Telemetry | `273e000b` |
| PPG ambient / IR / red | `273e000f` / `273e0010` / `273e0011` |

Decode, also to be verified:

- **EEG**, 256 Hz. 20-byte packet: 16-bit big-endian sequence, then 12 samples
  packed at 12 bits each. `uV = (raw - 0x800) * 125/256`.
- **PPG**, 64 Hz. 20-byte packet: 16-bit sequence, then 6 samples at 24 bits.
- **IMU**, 52 Hz. 20-byte packet: 16-bit sequence, then 3 triplets of 16-bit
  signed. Accel scale `1/16384` g, gyro scale `0.0074768` deg/s.

Each EEG channel arrives on its own characteristic with its own sequence
counter. They are aligned by sequence number, not by arrival order — Bluetooth
delivers notifications in whatever order it likes, and assuming otherwise would
shear the four channels against each other.

### `web/dsp.js` — quality and band power

A deliberate subset of `data/src/dsp.cpp` and `eeg_features.cpp`: what the
monitor screen needs to judge electrode seating, and nothing more.

```
assess(samples, sr)      -> {quality, rmsUv, railed, flat}   mirrors signal_quality.cpp
bandPowers(samples, sr)  -> {abs[5], rel[5], total}          Welch, Hann, nfft 256
```

**This is display only.** No number computed here reaches a CSV. The uploaded
rows carry raw samples, and every feature is recomputed by `elanora_data` from
those, exactly as the desktop path does. Otherwise band powers would silently
depend on which phone ran the session.

`assess()` must exclude NaN rather than count it as zero — the existing rule.
Counting a dropped packet as silence drags RMS down and mislabels a good
electrode as bad.

### `web/tone.js` — isochronic stimulus

A port of `collector/src/tone.cpp`. Carrier oscillator through a gain node whose
envelope is scheduled with `setValueCurveAtTime` ahead of real time — never
`setTimeout`, which would produce audible jitter and drift.

- `Stim`: regular gate per rate, summed over N rates, divided by N.
- `ControlJitter`: intervals from `mean ± 40%`, seeded PRNG, same pulse count.
- `ControlTone`: no gating, amplitude × `1/√2` to RMS-match.
- Raised-cosine edges of `ramp_ms`, because a hard edge is itself a startle.

The seeded PRNG must be reproducible from the session seed so a jitter round can
be reconstructed after the fact.

### `web/protocol.js` — phase state machine

Port of `TrialRunner`. Phases 30/30/30 baseline/stimulus/post, then 30 s rest.
Elapsed time comes from `AudioContext.currentTime`, the browser's equivalent of
the audio device clock the C++ runner uses — it is the accurate one, and it is
the same clock the gate envelope is scheduled against, so markers and stimulus
onset cannot drift apart.

Emits `baseline_start`, `stimulus_start`, `post_start`, `trial_end`.

### `web/upload.js` — CSV assembly and POST

Builds the four per-round files plus the trial row, POSTs to `/round`. Retries
with backoff; rounds queue in IndexedDB while offline so a wifi blip costs
nothing. A round is only dropped from the queue after the server acknowledges
the write.

### `server/` — `elanora_serve.exe`

Fifth executable. Depends on `common` (CSV) and `collector` (schedule,
protocol constants). Uses cpp-httplib, header-only, added via FetchContent.

```
GET  /                      static files from web/
GET  /schedule?seed=&count=&jitter=&tone=
                            JSON round order from build_schedule()
POST /round                 writes raw/<session>/<trial>_{eeg,ppg,imu,markers}.csv
                            appends trials.csv
POST /survey                appends surveys.csv
POST /session               appends sessions.csv
```

The server validates before writing: a round whose sample count is wildly wrong
for its duration is rejected with a reason, not stored. A malformed upload must
not be able to produce a file that later crashes `elanora_data`.

### `collector/include/elanora/collector/schema.hpp` — extracted

The raw CSV column headers currently exist only as string literals inside
`recorder.cpp`. The server would become a second place they are written, and two
copies of a schema drift. One header, consumed by both writers, so the desktop
and phone paths cannot disagree about what a valid dataset looks like.

This is the only change to existing code.

## Timestamps — a permanent deviation

The parent plan states as a Global Constraint that all timestamps are BrainFlow
board timestamps. **The phone path cannot honour this.** The Muse transmits a
16-bit per-packet sequence number and no clock of any kind.

Timestamps are therefore reconstructed:

```
t = t0 + (sequence * samples_per_packet + i) / sample_rate
```

anchored once per round, at `baseline_start`, to `AudioContext.currentTime`.
Every stream is anchored to that same origin, so markers, audio and samples
share one clock even though the headset supplies none.

Two consequences that must be handled, not hidden:

1. **Sequence wraps at 65536.** Unwrapped naively, a session longer than about
   51 minutes at 256 Hz produces timestamps that jump backwards. The decoder
   tracks wraps explicitly.
2. **A dropped packet leaves a sequence gap.** Those samples are emitted as NaN
   so the gap survives into the CSV, matching the existing "never bridge, never
   substitute zero" rule. A bridged gap looks like real signal; a zero-filled
   one looks like a physiological event.

Clock drift between the phone's audio clock and the headset's sampling clock is
not corrected. Over a 120-second round it is small relative to the 30-second
analysis windows, but it is a real limitation and belongs in any writeup.

## Error handling

| Failure | Behaviour |
|---|---|
| `navigator.bluetooth` undefined | Named diagnosis on the connect screen: not a secure context, or not Bluefy. Never a generic "connection failed". |
| Muse already connected elsewhere | "Close the Muse app" — the actual fix. |
| BLE drops mid-round | Round marked suspect, kept, flagged in the upload. Not silently discarded; a partial round is evidence about the session. |
| Tab suspended | Detected via gap in `AudioContext.currentTime` vs wall clock. Round marked suspect. |
| Server unreachable | Rounds queue in IndexedDB. Session continues. Banner shows the backlog. |
| Server rejects a round | Reason shown, round retained locally for manual export. |

## Testing

- **`web/selftest.html`** runs in the phone's browser, so it exercises the
  engine that will actually play. Asserts the same properties as
  `collector/tests/test_tone.cpp`: three conditions RMS-matched within 5%, gate
  onsets within 1 Hz of target, jitter control with matched pulse count but
  envelope peak prominence below 0.3, stacked rates never clipping. Runs through
  `OfflineAudioContext`, so it is fast and deterministic.
- **`dsp.js`** checked against fixed vectors whose expected values come from the
  existing C++ tests — a 10 Hz sine is alpha-dominant, relative powers sum to 1,
  doubling amplitude changes absolute but not relative power.
- **`muse.js`** decode tested against captured packet bytes, once real ones
  exist. This is why the inspector screen is built first.
- **`server/tests/`** Catch2: schedule JSON matches `build_schedule` for a given
  seed; a posted round produces files matching `schema.hpp`; a malformed round
  is rejected rather than written.

## Risks and build order

Three unknowns cannot be resolved from this machine, so they are settled first.

1. **The BLE decode is recalled, not verified.** Mitigation: the first screen
   built is a raw packet inspector showing lengths, sequence numbers and decoded
   values, so the table above is confirmed against real hardware before anything
   depends on it.
2. **Bluefy's secure-context behaviour is unknown.** If it enforces it, plain
   HTTP over the LAN exposes no `navigator.bluetooth` and the server needs TLS —
   which means OpenSSL, which is genuinely unpleasant on MSVC. This is a
   two-minute probe with a static page, and it decides the server's shape, so it
   happens before the server is written.
3. **iOS tab suspension.** Wake Lock plus explicit `Auto-Lock: Never`
   instructions plus gap detection, so a suspension is recorded as suspect
   rather than passing as clean data.

Build order:

1. **Probe** — static page in Bluefy, answer risk 2.
2. **BLE + inspector + monitor** — answer risk 1. Checkpoint: verified against
   the real headset before continuing.
3. **Server + upload** — schema conformance, round-trips into `elanora_data`.
4. **Protocol + tones + surveys** — the full 18-round session.

## Out of scope

- Android. The same code should work in Chrome, but it is not a target and will
  not be tested.
- Analysis on the phone. Evidence, models and the optimizer stay on the PC.
- Replacing the desktop collector. When a BLE adapter arrives, the desktop path
  remains the reference implementation.
