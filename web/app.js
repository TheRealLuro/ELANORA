// Monitor screen wiring.
//
// The collector's job is Task 13; this is the electrode check that has to pass
// before collecting is worth starting. It is the Milestone 1 gate from the
// parent plan, relocated to the phone because this project's PC has no
// Bluetooth radio and therefore cannot perform it.

import { Muse, EEG_NAMES } from "./muse.js";
import {
  decodeEeg, decodePpg, decodeImu,
  ACCEL_SCALE, GYRO_SCALE,
  EEG_PER_PACKET, PPG_PER_PACKET, IMU_PER_PACKET,
} from "./decode.js";
import { Stream } from "./ringbuffer.js";
import { assess, bandPowers, peakHz, BAND_NAMES } from "./dsp.js";

const SR_EEG = 256;
const SR_PPG = 64;
const SR_IMU = 52;

export const streams = {
  eeg: Object.fromEntries(EEG_NAMES.map((n) => [n, new Stream(SR_EEG, EEG_PER_PACKET)])),
  ppgRed: new Stream(SR_PPG, PPG_PER_PACKET),
  ppgIr: new Stream(SR_PPG, PPG_PER_PACKET),
  ppgAmbient: new Stream(SR_PPG, PPG_PER_PACKET),
  accel: new Stream(SR_IMU, IMU_PER_PACKET),
  gyro: new Stream(SR_IMU, IMU_PER_PACKET),
};

export const muse = new Muse();

const el = (id) => document.getElementById(id);
const now = () => performance.now() / 1000;

function setStatus(text, cls = "") {
  const s = el("status");
  s.textContent = text;
  s.className = cls;
}

muse.onRaw((name, dv) => {
  try {
    if (EEG_NAMES.includes(name)) {
      const { seq, samples } = decodeEeg(dv);
      streams.eeg[name].push(seq, samples);
    } else if (name === "ppgRed" || name === "ppgIr" || name === "ppgAmbient") {
      const { seq, samples } = decodePpg(dv);
      streams[name].push(seq, samples);
    } else if (name === "accel" || name === "gyro") {
      const scale = name === "accel" ? ACCEL_SCALE : GYRO_SCALE;
      const { seq, samples } = decodeImu(dv, scale);
      // Flattened x,y,z per sample; the CSV writer splits them back out.
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
  } catch (e) {
    el("err").textContent = e.message;
    setStatus("not connected", "err");
    btn.disabled = false;
  }
};

// 4 Hz. Fast enough to feel live while seating an electrode, slow enough that
// the FFTs do not compete with the Bluetooth callbacks for the main thread.
setInterval(() => {
  if (!muse.connected) return;
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
  el("electrodes").innerHTML = rows.join("");

  // Dropped packets are reported rather than hidden. A rising count means the
  // phone is too far from the headset, and that is fixable in the moment.
  el("drops").textContent = dropped
    ? `${dropped} samples lost to dropped packets`
    : "";
}, 250);
