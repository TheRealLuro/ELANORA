# Changelog

Notable changes, newest first. Entries explain *why* a change was made, because
several of these exist only because a specific failure happened during real
recording.

## Collector reliability and the full frequency range

### Recording could fail silently for a whole session

Five recorded rounds failed to upload and the app said nothing until the
session was over. The cause was not the network: every `POST /round` returned
`401`, because the page had been opened without the write token in its URL and
so had nothing to send.

An operator could therefore record for 36 minutes into a server that was never
going to accept a byte of it.

- **Start now refuses unless a real authenticated write succeeds**, against a
  new `POST /writecheck` that writes nothing and answers with a status code.
- **Queue depth is shown during the session**, so a failure surfaces in the
  round it happens rather than at the end.
- **The upload queue can be retried.** It previously only flushed when a new
  round was enqueued, so once a session ended there was no path out at all.

### Queued rounds could be stranded permanently

Browser storage is scoped per origin. When a tunnel hostname changed, the
queue's only exit — `POST` to the origin the page was loaded from — pointed at
an address that no longer existed, and the rounds could not be reached from any
other. Eight recorded rounds were lost this way.

- **Save to device** writes everything queued to a zip of CSVs, at the same
  paths the server would have used, via the iOS share sheet. Unzip into
  `data/datasets/` and the result is indistinguishable from a normal upload.
- **`elanora_serve --import <file>`** replays a rescued export straight into the
  dataset, through the same parsing and validation the HTTP path uses rather
  than a second, less-tested one.

Stored rather than deflated: a correct deflate is a lot of code to get wrong,
and a recovery path is the worst place for a subtle bug.

### Full audible range, delivered two ways

The stimulus range now reaches 11.6 kHz, which changes what a frequency
physically *is* partway up the scale. Hearing starts near 20 Hz, so 0.5 Hz can
only exist as a pulse rate gating an audible carrier; above 20 Hz the frequency
can be the tone itself, and gating becomes the awkward option instead — a 1 kHz
gate on a 440 Hz carrier is not a rhythm, it is noise.

Three session ranges tile `f(k) = 0.5 × √2^k`, k = 0..29, joining end to end
with no gap and no overlap, split **at the regime boundary** so no session mixes
the two kinds of stimulus:

| Range | Span | Frequencies | Delivery | Rounds |
|---|---|---|---|---|
| Rhythm | 0.5 – 16 Hz | 11 | rate | 15 |
| Low pitch | 22.6 – 362 Hz | 9 | pitch | 13 |
| High pitch | 512 Hz – 11.6 kHz | 10 | pitch | 14 |

The cost is real and belongs to the operator: range now covaries with session.
Randomise which range each subject starts with, and run every range across
several subjects.

Consequences worth knowing:

- **Rhythmic / Wave / Swell only apply below 20 Hz.** Above the crossover there
  is no rhythm to shape. The C++↔browser conformance test caught this within
  minutes, when a 45 Hz wave case stopped matching.
- **Carrier pitch is a session-level setting**, held constant throughout. It is
  the vehicle, not the variable: a pitch that moved with the rate would make the
  two impossible to tell apart.

### A third envelope, and equal loudness across rates

- **Swell** — a continuous tone at 35% modulation depth that never reaches
  silence. Gated and Wave both give the ear an onset, so a response to either
  could be a startle rather than entrainment; a rate that works under Swell is
  driving the periodic amplitude change itself.
- **Every stimulus rate is now equally loud within 1%.** The filter that rounds
  gate edges removes more energy the more edges there are, so a 45 Hz round was
  **17% quieter** than a 0.5 Hz one — loudness covarying with the independent
  variable, which is exactly the confound RMS matching exists to eliminate.

### iOS silenced the stimulus, and a silent session looks perfect

A bare `AudioContext` on iOS obeys the hardware silent switch, so Web Audio went
mute while video and music apps kept playing. A silenced session still records
clean EEG, correct markers and the right round lengths, with **no stimulus in
any of it** — and nothing downstream can detect that.

The page now forces the louder audio session category, and the setup screen
plays the real stimulus as a 0.5–45 Hz sweep so the operator can confirm it by
ear. Software cannot check this: iOS mutes downstream of everything the page can
observe, so an analyser would report a healthy signal into a dead speaker.

### Vitals read as confidently wrong

The phone's heart and breathing estimates were *simplified* from the C++ rather
than ported from it. Dropping the Butterworth filter, the spectral cross-check
and the confidence terms did not produce a rougher number — it produced a
confident wrong one.

Tests against synthetic signals at known rates caught an error the original had
no way to notice: **a 50 bpm pulse read as 99.1**, almost exactly double,
because real PPG has a dicrotic notch that a threshold detector counts as a
second beat. That error is *regular*, so the regularity term rated the doubled
reading as excellent quality. An independent spectral estimate now resolves the
octave, and every figure displays its confidence.

### Two timebase bugs that made the headset read as dead

- Timestamps were computed from the **absolute** packet sequence number. Every
  test pushed sequence 0 first, so the term was zero and the arithmetic looked
  right. A real headset's counter is free-running: starting around 30000 put
  every timestamp 23 minutes in the future, every window query came back empty,
  and four healthy electrodes read as flat.
- The IMU stream stores three axes per reading, interleaved, so it counts values
  rather than readings — but was given the rate in readings. IMU time ran three
  times too slow, and a breathing rate a third of its true value is low but not
  absurd, so it would have passed the sanity check and become a finding.

### Partial sessions are kept, not discarded

A round is saved only after the whole cycle — baseline, stimulus, post, and the
questions. The upload used to fire at the end of the post phase, so stopping
during the questions left a trial row with no survey beside it: a round that did
not complete the cycle, stored as though it had, with nothing downstream able to
tell.

### Synthetic fixtures had leaked into the real dataset

Four test sessions with timestamps starting at exactly `1000.000000` were being
displayed as collected data — 56 fabricated trial rows. Moved to
`data/fixtures/` and purged from the tables.

---

## Diagnostic notes

Kept because each cost real time to work out.

- **Traffic through `cloudflared` arrives from `127.0.0.1`**, because the tunnel
  is a local proxy. Filtering the server log for non-localhost peers to find the
  phone therefore finds nothing, and reads as "the phone never connected" when
  it had connected all along.
- **Quick tunnel hostnames are never reissued.** A restart means a new origin,
  and browser storage does not follow. Use the save-to-device export before
  changing tunnels with rounds still queued.
- **The Muse 2's USB port is a control channel, not a data channel.** It answers
  `v1`, `s`, `h` and `p50` with JSON — useful for checking firmware and battery
  without Bluetooth — but `d` starts the stream on the BLE characteristics and
  nothing arrives over the cable.
- **Our own compiler flags broke a dependency.** `/permissive-` and
  `/Zc:preprocessor` were set before the FetchContent dependencies, so vendored
  sources inherited them; SimpleBLE compiles with `/WX`, and both flags turned
  its quirks into hard errors. No `simpleble-c.dll` was produced, and since
  BrainFlow loads it with `LoadLibrary` nothing linked it either — so the DLL
  copy step silently shipped without it.
