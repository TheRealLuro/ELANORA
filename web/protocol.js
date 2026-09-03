// The round state machine.
//
// A port of TrialRunner. Time is passed in by the caller rather than read from
// a clock here: the caller supplies AudioContext.currentTime, which is the same
// clock the stimulus buffer is scheduled against, so markers and stimulus onset
// cannot drift apart. It also makes the machine testable with plain numbers.

export const PHASES = ["baseline", "stimulus", "post", "rest"];

export class Round {
  #d;
  #t0 = 0;
  #markers = [];
  #emitted = new Set();

  constructor(durations = {}) {
    const { baseline = 30, stimulus = 30, post = 30, rest = 30 } = durations;
    this.#d = { baseline, stimulus, post, rest };
  }

  get durations() { return { ...this.#d }; }
  get total() {
    return this.#d.baseline + this.#d.stimulus + this.#d.post + this.#d.rest;
  }
  get startedAt() { return this.#t0; }

  // The absolute time the stimulus buffer must be started at. Derived from the
  // same origin as the markers so the two cannot disagree.
  get stimulusAt() { return this.#t0 + this.#d.baseline; }

  start(t0) {
    this.#t0 = t0;
    this.#markers = [];
    this.#emitted = new Set();
  }

  #emit(event, ts) {
    if (this.#emitted.has(event)) return;
    this.#emitted.add(event);
    this.#markers.push({ ts, event });
  }

  phaseAt(t) {
    const e = t - this.#t0;
    const { baseline, stimulus, post, rest } = this.#d;

    // Markers are stamped at the boundary, not at the polling time, so a slow
    // frame cannot move a marker. The phase boundary is a property of the
    // schedule; when we noticed it is not.
    if (e >= 0) this.#emit("baseline_start", this.#t0);
    if (e >= baseline) this.#emit("stimulus_start", this.#t0 + baseline);
    if (e >= baseline + stimulus) {
      this.#emit("post_start", this.#t0 + baseline + stimulus);
    }
    if (e >= baseline + stimulus + post) {
      this.#emit("trial_end", this.#t0 + baseline + stimulus + post);
    }

    if (e < 0) return "baseline";
    if (e < baseline) return "baseline";
    if (e < baseline + stimulus) return "stimulus";
    if (e < baseline + stimulus + post) return "post";
    if (e < baseline + stimulus + post + rest) return "rest";
    return "done";
  }

  // Seconds left in the current phase, for the countdown.
  remainingIn(t) {
    const e = t - this.#t0;
    const { baseline, stimulus, post, rest } = this.#d;
    const bounds = [baseline, baseline + stimulus, baseline + stimulus + post,
                    baseline + stimulus + post + rest];
    for (const b of bounds) if (e < b) return b - e;
    return 0;
  }

  markers() { return this.#markers.slice(); }
}
