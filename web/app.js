// Screen router and wiring.
//
// The monitor is the electrode check that has to pass before collecting is
// worth starting -- the Milestone 1 gate from the parent plan, relocated to the
// phone because this project's PC has no Bluetooth radio and cannot perform it.
// The collector is the 18-round session.

import { Muse, EEG_NAMES } from "./muse.js";
import {
  decodeEeg, decodePpg, decodeImu,
  ACCEL_SCALE, GYRO_SCALE,
  EEG_PER_PACKET, PPG_PER_PACKET, IMU_PER_PACKET,
} from "./decode.js";
import { Stream } from "./ringbuffer.js";
import { assess, bandPowers, peakHz, BAND_NAMES } from "./dsp.js";
import { Session } from "./collector.js";

const SR_EEG = 256, SR_PPG = 64, SR_IMU = 52;

const streams = {
  eeg: Object.fromEntries(EEG_NAMES.map((n) => [n, new Stream(SR_EEG, EEG_PER_PACKET)])),
  ppgRed: new Stream(SR_PPG, PPG_PER_PACKET),
  ppgIr: new Stream(SR_PPG, PPG_PER_PACKET),
  ppgAmbient: new Stream(SR_PPG, PPG_PER_PACKET),
  // Three axes per sample, so the ring buffer sees 3x the samples per packet.
  accel: new Stream(SR_IMU, IMU_PER_PACKET * 3),
  gyro: new Stream(SR_IMU, IMU_PER_PACKET * 3),
};

const muse = new Muse();
const session = new Session(streams);

const el = (id) => document.getElementById(id);
const now = () => performance.now() / 1000;

function setStatus(text, cls = "") {
  const s = el("status");
  s.textContent = text;
  s.className = cls;
}

function show(name) {
  for (const id of ["monitor", "collect", "run", "survey", "done"]) {
    el(`screen-${id}`).hidden = id !== name;
  }
  // The tab strip only makes sense between the two entry screens; during a
  // running session there is nowhere else to go.
  document.querySelector("nav.tabs").hidden = (name === "run" || name === "survey");
}

// ------------------------------------------------------------------ device

muse.onRaw((name, dv) => {
  try {
    if (EEG_NAMES.includes(name)) {
      const { seq, samples } = decodeEeg(dv);
      streams.eeg[name].push(seq, samples);
    } else if (name === "ppgRed" || name === "ppgIr" || name === "ppgAmbient") {
      const { seq, samples } = decodePpg(dv);
      streams[name].push(seq, samples);
    } else if (name === "accel" || name === "gyro") {
      const { seq, samples } = decodeImu(dv, name === "accel" ? ACCEL_SCALE : GYRO_SCALE);
      streams[name].push(seq, samples.flat());
    }
  } catch {
    // A malformed packet is dropped rather than allowed to throw out of a
    // Bluetooth event handler, where the rejection would be invisible.
  }
});

muse.onLost(() => {
  setStatus("headset disconnected", "err");
  el("connect").disabled = false;
  el("connect").textContent = "Reconnect headset";
  el("start").disabled = true;
  el("start").textContent = "Connect the headset first";
});

el("connect").onclick = async () => {
  const btn = el("connect");
  el("err").textContent = "";
  btn.disabled = true;
  try {
    await muse.connect();
    // Every stream shares one origin, so markers, audio and samples stay on the
    // same timebase even though the headset supplies no clock of its own.
    const t0 = now();
    for (const s of Object.values(streams.eeg)) s.anchor(t0);
    for (const k of ["ppgRed", "ppgIr", "ppgAmbient", "accel", "gyro"]) {
      streams[k].anchor(t0);
    }
    await muse.start();
    setStatus("streaming", "live");
    btn.textContent = `connected — ${muse.name || "Muse"}`;
    el("hint").textContent = "Give it about ten seconds to settle.";
    el("start").disabled = false;
    el("start").textContent = "Start session";
  } catch (e) {
    el("err").textContent = e.message;
    setStatus("not connected", "err");
    btn.disabled = false;
  }
};

// ----------------------------------------------------------------- monitor

function electrodeRows() {
  const t = now();
  const rows = [];
  let dropped = 0;
  for (const name of EEG_NAMES) {
    const stream = streams.eeg[name];
    const { v } = stream.slice(t - 2, t);
    dropped += stream.dropped;
    const q = assess(v, SR_EEG);
    const bp = bandPowers(v, SR_EEG);
    const dom = bp.rel.indexOf(Math.max(...bp.rel));
    const label = bp.total > 0 ? BAND_NAMES[dom] : "—";
    const pk = peakHz(v, SR_EEG);
    rows.push(
      `<tr><td class="name">${name}</td>` +
      `<td class="q"><span class="dot ${q.quality}"></span>` +
      `<span class="${q.quality}">${q.quality}</span></td>` +
      `<td class="num">${q.flat ? "flat" : q.railed ? "railed" : q.rmsUv.toFixed(1) + " µV"}</td>` +
      `<td class="num">${label}${pk ? " " + pk.toFixed(1) + " Hz" : ""}</td></tr>`
    );
  }
  return { html: rows.join(""), dropped };
}

setInterval(() => {
  if (!muse.connected) return;
  const { html, dropped } = electrodeRows();
  const target = el("screen-run").hidden ? "electrodes" : "run-electrodes";
  el(target).innerHTML = html;
  const drops = el("screen-run").hidden ? "drops" : "run-drops";
  // Dropped packets are reported rather than hidden. A rising count means the
  // phone is too far from the headset, and that is fixable in the moment.
  el(drops).textContent = dropped ? `${dropped} samples lost to dropped packets` : "";
}, 250);

// -------------------------------------------------------------------- tabs

for (const tab of document.querySelectorAll(".tab")) {
  tab.onclick = () => {
    for (const t of document.querySelectorAll(".tab")) t.classList.remove("active");
    tab.classList.add("active");
    show(tab.dataset.screen);
  };
}

// ------------------------------------------------------------------- setup

const reseed = () => {
  session.seed = Math.floor(Math.random() * 4294967296);
  el("seed-val").textContent = session.seed;
};
el("reseed").onclick = reseed;
el("subject").oninput = () => {
  session.subject = el("subject").value.trim() || "P01";
};

session.fetchSchedule().then((rounds) => {
  const mins = Math.round(rounds.length * 120 / 60);
  el("protocol").textContent =
    `${rounds.length} rounds · ${mins} min · 120 s per round`;
}).catch(() => {
  el("protocol").textContent = "server unreachable — using the cached schedule";
  session.loadCachedSchedule();
});

// iOS releases the wake lock whenever the page is hidden, so it is re-acquired
// on every return to visibility. It is best-effort: the Auto-Lock instruction
// on the setup screen is the real defence, because a locked screen suspends
// Bluetooth and audio and the round is lost either way.
let wakeLock = null;
async function holdScreen() {
  try { wakeLock = await navigator.wakeLock.request("screen"); } catch { /* unsupported */ }
}
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "visible" && session.running) holdScreen();
});

el("start").onclick = async () => {
  el("setup-err").textContent = "";
  try {
    // Must happen inside the gesture handler: iOS will not start an
    // AudioContext outside one, and the failure is silent.
    await session.initAudio();
    await holdScreen();
    session.subject = el("subject").value.trim() || "P01";
    if (!session.schedule.length) await session.fetchSchedule();
    show("run");
    await session.start();
  } catch (e) {
    el("setup-err").textContent = e.message;
    show("collect");
  }
};

el("abort").onclick = () => {
  session.abort();
  show("collect");
};

// --------------------------------------------------------------- run + tick

session.onTick = (phase, remaining, i) => {
  el("run-round").textContent = `Round ${i + 1} of ${session.total}`;
  el("run-phase").textContent = phase;
  el("run-count").textContent = Math.ceil(remaining);

  const d = session.durations;
  const spans = { baseline: d.baseline, stimulus: d.stimulus, post: d.post, rest: d.rest };
  const span = spans[phase] || 1;
  el("run-fill").style.width = `${100 * (1 - remaining / span)}%`;

  // Deliberately says nothing about the condition. The subject is also the
  // operator here, and telling them which rounds are controls would
  // contaminate the comparison the controls exist to protect.
  el("run-hint").textContent = phase === "post"
    ? "Keep still. The sound has stopped; this part still counts."
    : "Sit still, eyes closed.";
};

// ------------------------------------------------------------------ survey

const survey = {
  relaxation: 4, alertness: 4, pleasantness: 4, discomfort: 0,
  breathing: "same", rhythm: "", breaths: -1,
  jaw: false, moved: false, eyes: false, swallow: false, noise: false, note: "",
};

for (const [id, key, out] of [
  ["s-relax", "relaxation", "o-relax"],
  ["s-alert", "alertness", "o-alert"],
  ["s-pleas", "pleasantness", "o-pleas"],
  ["s-disc", "discomfort", "o-disc"],
]) {
  el(id).oninput = () => {
    survey[key] = Number(el(id).value);
    el(out).textContent = el(id).value;
  };
}

function segment(containerId, key) {
  const box = el(containerId);
  for (const b of box.querySelectorAll("button")) {
    b.onclick = () => {
      for (const o of box.querySelectorAll("button")) o.classList.remove("on");
      b.classList.add("on");
      survey[key] = b.dataset.v;
      refreshSubmit();
    };
  }
}
segment("s-breath", "breathing");
segment("s-rhythm", "rhythm");

for (const b of el("s-artifacts").querySelectorAll("button")) {
  b.onclick = () => {
    b.classList.toggle("on");
    survey[b.dataset.v] = b.classList.contains("on");
  };
}
el("s-note").oninput = () => { survey.note = el("s-note").value; };

// Submit stays blocked until the manipulation check is answered. A blank
// heard_rhythm cannot enter the dataset, because it is the field that shows
// whether the jitter control actually reads as non-rhythmic.
function refreshSubmit() {
  const ok = survey.rhythm !== "";
  el("survey-submit").disabled = !ok;
  el("survey-submit").textContent = ok ? "Next round" : "Answer the rhythm question";
}

let resolveSurvey = null;
session.onSurvey = (i) => new Promise((resolve) => {
  resolveSurvey = resolve;
  el("survey-round").textContent = `After round ${i + 1}`;
  // Reset the answers that must be given fresh each round; sliders keep their
  // position because a subject who felt the same twice should not have to
  // re-enter it, but the rhythm answer is always cleared.
  survey.rhythm = "";
  for (const o of el("s-rhythm").querySelectorAll("button")) o.classList.remove("on");
  for (const o of el("s-artifacts").querySelectorAll("button")) o.classList.remove("on");
  for (const k of ["jaw", "moved", "eyes", "swallow", "noise"]) survey[k] = false;
  survey.note = "";
  el("s-note").value = "";
  refreshSubmit();
  show("survey");
});

el("survey-submit").onclick = async () => {
  const i = session.roundIndex;
  el("survey-submit").disabled = true;
  await session.submitSurvey(i, survey);
  show("run");
  resolveSurvey?.();
  resolveSurvey = null;
};

// -------------------------------------------------------------------- done

session.onDone = async () => {
  show("done");
  el("done-count").textContent = `${session.total} rounds`;
  const left = await session.pendingUploads;
  el("done-hint").textContent = left
    ? `${left} rounds still uploading — keep this page open until it reaches zero.`
    : "Everything uploaded. Run elanora_data on the PC to build the features.";
  try { wakeLock?.release(); } catch { /* already gone */ }
  wakeLock = null;
};

session.onError = (msg) => { el("setup-err").textContent = msg; };
el("done-ok").onclick = () => show("collect");

show("monitor");
