// The session runner.
//
// Same shape as the desktop collector: type a name, press start, and the
// machine runs 18 rounds on its own with the survey opening by itself after
// each one. The operator is also the subject here, which changes one thing
// materially -- the condition is NEVER shown. On the desktop a second person
// reads the screen; on a phone the person whose alpha is being measured is
// looking at it, and telling them "this one is a control" would contaminate
// the measurement the controls exist to protect.
//
// Timebase. Sample streams are anchored to performance.now(), which is
// monotonic and exists before any AudioContext does. Audio is scheduled on
// ctx.currentTime, which is the accurate clock for onset. The two are related
// by one offset measured when the context is created, so markers, samples and
// stimulus onset all trace back to a single origin.

import { EEG_NAMES } from "./muse.js";
import { Round } from "./protocol.js";
import { renderStimulus } from "./tone.js";
import { toCsv, UploadQueue, authHeaders } from "./upload.js";

const SR_EEG = 256;
const SR_PPG = 64;
const SR_IMU = 52;

// Filters need room to settle; 2 s rather than 1 because the 0.1 Hz breathing
// band-pass has a long impulse response and would otherwise eat real data.
const MARGIN_S = 2.0;

export class Session {
  #streams;
  #ctx = null;
  #audioEpoch = 0;
  #queue = new UploadQueue("");
  #source = null;

  constructor(streams) {
    this.#streams = streams;
    this.schedule = [];
    this.roundIndex = 0;
    this.sessionId = "";
    this.subject = "P01";
    this.ageBand = "";
    // "gated" (isochronic) or "wave" (sinusoidal AM).
    this.envelope = "gated";
    this.trialNumber = 1;
    this.seed = 84120;
    this.durations = { baseline: 30, stimulus: 30, post: 30, rest: 30 };
    this.suspect = false;
    this.running = false;
    // Set by the UI; called with (phase, remaining, roundIndex).
    this.onTick = null;
    this.onSurvey = null;
    this.onDone = null;
    this.onError = null;
  }

  get pendingUploads() { return this.#queue.pending(); }
  get total() { return this.schedule.length; }

  // perf-time seconds, the timebase every stored sample uses.
  #now() { return performance.now() / 1000; }

  async fetchSchedule() {
    const url = `/schedule?seed=${this.seed}`;
    const res = await fetch(url);
    if (!res.ok) throw new Error(`schedule: ${res.status}`);
    const json = await res.json();
    this.schedule = json.rounds;
    // Cached so a wifi drop after the session starts cannot abort the run.
    localStorage.setItem("elanora.schedule", JSON.stringify(json.rounds));
    return this.schedule;
  }

  loadCachedSchedule() {
    const raw = localStorage.getItem("elanora.schedule");
    if (raw) this.schedule = JSON.parse(raw);
    return this.schedule;
  }

  #stamp() {
    const d = new Date();
    const p = (n, w = 2) => String(n).padStart(w, "0");
    return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-` +
           `${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
  }

  trialId(i) {
    return `${this.sessionId}_R${String(i + 1).padStart(2, "0")}`;
  }

  // iOS will not start audio outside a user gesture, so this must be called
  // synchronously from the Start button's handler.
  async initAudio() {
    if (!this.#ctx) {
      this.#ctx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (this.#ctx.state === "suspended") await this.#ctx.resume();
    this.#audioEpoch = this.#now() - this.#ctx.currentTime;
    return this.#ctx;
  }

  // perf-time -> audio-clock time.
  #toAudio(t) { return t - this.#audioEpoch; }

  async start() {
    this.sessionId = `${this.subject}_S${String(this.trialNumber).padStart(2, "0")}_${this.#stamp()}`;
    this.running = true;
    this.roundIndex = 0;

    // subjects.csv is in the schema and nothing was writing it. A duplicate
    // row per session is harmless -- the analysis reads the subject list, not
    // its cardinality -- and it keeps the file honest without the phone having
    // to know whether this subject already exists.
    await this.#post("/subject",
                     { subject_id: this.subject, age_band: this.ageBand, notes: "" });
    await this.#postSession();
    await this.#runAll();
  }

  // Small keyed POST, used for the metadata tables. A failure here never stops
  // a subject who is already wearing the headset: every one of these rows can
  // be reconstructed from the trial rows afterwards.
  async #post(path, fields) {
    let body = "";
    for (const [k, v] of Object.entries(fields)) body += `${k}: ${v}
`;
    try {
      await fetch(path, { method: "POST", headers: authHeaders(), body });
      return true;
    } catch {
      return false;
    }
  }

  async #postSession() {
    const meta = {
      session_id: this.sessionId,
      subject_id: this.subject,
      trial_number: this.trialNumber,
      date: this.sessionId.split("_")[2].slice(0, 8),
      // Recorded so the analysis can tell the two envelope shapes apart. It
      // rides in stim_mode rather than a new column, so nothing downstream has
      // to change to read a dataset that mixes them.
      stim_mode: `sweep_${this.envelope}`,
      carrier_hz: (440).toFixed(6),
      duty_cycle: (0.5).toFixed(6),
      baseline_s: this.durations.baseline.toFixed(6),
      stimulus_s: this.durations.stimulus.toFixed(6),
      post_s: this.durations.post.toFixed(6),
      rest_s: this.durations.rest.toFixed(6),
      order_seed: this.seed,
      round_count: this.schedule.length,
      quality_override: "0",
    };
    let body = "";
    for (const [k, v] of Object.entries(meta)) body += `${k}: ${v}\n`;
    try {
      await fetch("/session", { method: "POST", headers: authHeaders(), body });
    } catch {
      // The session row can be reconstructed from the trial rows, so a failure
      // here must not stop a subject who is already wearing the headset.
    }
  }

  async #runAll() {
    for (let i = 0; i < this.schedule.length && this.running; i++) {
      this.roundIndex = i;
      await this.#runRound(i);
      if (!this.running) break;
      // The survey occupies the rest window rather than adding to it, which is
      // what keeps 18 rounds inside 36 minutes.
      if (this.onSurvey) await this.onSurvey(i);
    }
    this.running = false;
    this.onDone?.();
  }

  async #runRound(i) {
    const spec = this.schedule[i];
    const ctx = await this.initAudio();
    const round = new Round(this.durations);

    // Rendered before the round starts. Building a 30 s buffer takes real time,
    // and doing it at the boundary would delay stimulus onset.
    const buffer = renderStimulus(ctx, {
      condition: spec.condition,
      hz: Number(spec.frequency_hz),
      jitterMeanHz: Number(spec.jitter_mean_hz) || 10,
      seed: this.seed + i,
      seconds: this.durations.stimulus,
      envelope: this.envelope,
    });

    const t0 = this.#now();
    round.start(t0);

    // Scheduled once, up front, on the audio clock. Sample-accurate, and it
    // cannot be moved by a slow frame later.
    this.#source = ctx.createBufferSource();
    this.#source.buffer = buffer;
    this.#source.connect(ctx.destination);
    this.#source.start(this.#toAudio(round.stimulusAt));

    const wallStart = t0;
    const audioStart = ctx.currentTime;
    this.suspect = false;

    await new Promise((resolve) => {
      const tick = () => {
        if (!this.running) return resolve();
        const t = this.#now();
        const phase = round.phaseAt(t);
        this.onTick?.(phase, round.remainingIn(t), i);

        // ctx.currentTime stalls while the tab is suspended but the wall clock
        // does not. A divergence means the round lost real time, and a round
        // that lost time is not a clean recording.
        const drift = Math.abs((t - wallStart) - (ctx.currentTime - audioStart));
        if (drift > 0.5) this.suspect = true;

        if (phase === "done" || t - t0 >= round.total - this.durations.rest) {
          return resolve();
        }
        requestAnimationFrame(tick);
      };
      requestAnimationFrame(tick);
    });

    try { this.#source.stop(); } catch { /* already ended */ }
    this.#source = null;

    if (this.running) await this.#uploadRound(i, spec, round);
  }

  #sliceCsv(stream, header, t0, t1, columns) {
    const { t, v } = stream.slice(t0, t1);
    const rows = [];
    const n = columns;
    for (let k = 0; k + n <= v.length; k += n) {
      const row = [t[k]];
      for (let c = 0; c < n; c++) row.push(v[k + c]);
      rows.push(row);
    }
    return toCsv(header, rows);
  }

  async #uploadRound(i, spec, round) {
    const markers = round.markers();
    if (!markers.length) return;

    const t0 = markers[0].ts - MARGIN_S;
    const t1 = markers[markers.length - 1].ts + MARGIN_S;

    // EEG: four separate streams that have to be interleaved into one table.
    // They are aligned on timestamp, not on array position -- each channel has
    // its own sequence counter and its own dropped packets, so position would
    // shear them against each other.
    const eegRows = [];
    const base = this.#streams.eeg[EEG_NAMES[0]].slice(t0, t1);
    const others = EEG_NAMES.slice(1).map((n) => this.#streams.eeg[n].slice(t0, t1));
    for (let k = 0; k < base.t.length; k++) {
      const row = [base.t[k], base.v[k]];
      for (const o of others) row.push(k < o.v.length ? o.v[k] : NaN);
      eegRows.push(row);
    }

    const ppgRed = this.#streams.ppgRed.slice(t0, t1);
    const ppgIr = this.#streams.ppgIr.slice(t0, t1);
    const ppgAmb = this.#streams.ppgAmbient.slice(t0, t1);
    const ppgRows = ppgRed.t.map((ts, k) => [
      ts, ppgRed.v[k],
      k < ppgIr.v.length ? ppgIr.v[k] : NaN,
      k < ppgAmb.v.length ? ppgAmb.v[k] : NaN,
    ]);

    const accel = this.#streams.accel.slice(t0, t1);
    const gyro = this.#streams.gyro.slice(t0, t1);
    const imuRows = [];
    for (let k = 0, s = 0; k + 3 <= accel.v.length; k += 3, s++) {
      imuRows.push([
        accel.t[k], accel.v[k], accel.v[k + 1], accel.v[k + 2],
        gyro.v[k] ?? NaN, gyro.v[k + 1] ?? NaN, gyro.v[k + 2] ?? NaN,
      ]);
    }

    const meta = {
      session_id: this.sessionId,
      trial_id: this.trialId(i),
      subject_id: this.subject,
      condition: spec.condition,
      round_index: i + 1,
      frequency_hz: Number(spec.frequency_hz).toFixed(6),
      jitter_mean_hz: Number(spec.jitter_mean_hz).toFixed(6),
      suspect: this.suspect ? "1" : "0",
    };

    await this.#queue.enqueue(meta, {
      eeg: toCsv(["timestamp", ...EEG_NAMES], eegRows),
      ppg: toCsv(["timestamp", "ppg_red", "ppg_ir", "ppg_ambient"], ppgRows),
      imu: toCsv(["timestamp", "ax", "ay", "az", "gx", "gy", "gz"], imuRows),
      markers: toCsv(["timestamp", "event"], markers.map((m) => [m.ts, m.event])),
    });
  }

  async submitSurvey(i, s) {
    const fields = {
      trial_id: this.trialId(i),
      session_id: this.sessionId,
      subject_id: this.subject,
      round_index: i + 1,
      relaxation: s.relaxation,
      alertness: s.alertness,
      pleasantness: s.pleasantness,
      discomfort: s.discomfort,
      breathing_perceived: s.breathing,
      // Blank, never 0, when uncounted. Zero breaths in 90 seconds is a number,
      // and a downstream mean would happily average it in.
      breaths_self_count: s.breaths >= 0 ? s.breaths : "",
      heard_rhythm: s.rhythm,
      artifact_jaw: s.jaw ? 1 : 0,
      artifact_move: s.moved ? 1 : 0,
      artifact_eyes: s.eyes ? 1 : 0,
      artifact_swallow: s.swallow ? 1 : 0,
      artifact_noise: s.noise ? 1 : 0,
      note: (s.note || "").replace(/[\r\n]+/g, " "),
    };
    let body = "";
    for (const [k, v] of Object.entries(fields)) body += `${k}: ${v}\n`;
    try {
      await fetch("/survey", { method: "POST", headers: authHeaders(), body });
    } catch (e) {
      this.onError?.("survey not uploaded: " + e.message);
    }
  }

  abort() {
    this.running = false;
    try { this.#source?.stop(); } catch { /* not started */ }
  }
}
