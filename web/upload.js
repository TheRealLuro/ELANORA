// CSV assembly and the durable upload queue.
//
// A round is persisted to IndexedDB before it is posted and deleted only after
// the server acknowledges. A session therefore survives a wifi drop, a page
// reload, and a server restart -- which matters because losing round 17 of 18
// costs the subject forty minutes.

const DB_NAME = "elanora";
const STORE = "rounds";

// Every value at six decimal places, matching fmt6 in the C++ so a number reads
// identically whichever writer produced it.
function fmt6(x) {
  if (!Number.isFinite(x)) return "";   // a gap, not a value
  return x.toFixed(6);
}

// NaN becomes an empty field, never the text "NaN". An empty field is how a gap
// survives into the CSV; the string would be read as a value by anything that
// parses loosely.
export function toCsv(header, rows) {
  let out = header.join(",") + "\n";
  for (const row of rows) {
    for (let i = 0; i < row.length; i++) {
      if (i) out += ",";
      const v = row[i];
      out += typeof v === "number" ? fmt6(v) : String(v ?? "");
    }
    out += "\n";
  }
  return out;
}

// The wire format the server parses: "key: value" headers, a blank line, then
// length-prefixed sections. No escaping, because the payload is hundreds of
// kilobytes of commas and newlines and an escaping bug would corrupt samples in
// ways that still parse as valid CSV.
export function envelope(meta, sections) {
  let head = "";
  for (const [k, v] of Object.entries(meta)) head += `${k}: ${v}\n`;
  head += "\n";

  let body = head;
  for (const [name, csv] of Object.entries(sections)) {
    if (!csv) continue;
    // Byte length, not string length: a UTF-8 character would make the two
    // differ and the server reads by count.
    const bytes = new TextEncoder().encode(csv).length;
    body += `@${name} ${bytes}\n${csv}`;
  }
  return body;
}

function openDb() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => {
      req.result.createObjectStore(STORE, { keyPath: "trial_id" });
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function tx(db, mode, fn) {
  return new Promise((resolve, reject) => {
    const t = db.transaction(STORE, mode);
    const req = fn(t.objectStore(STORE));
    t.oncomplete = () => resolve(req?.result);
    t.onerror = () => reject(t.error);
  });
}

// The write token, taken from the page URL once and kept.
//
// A page reached by scanning a QR carries "?k=..." and nothing else, so the
// token is read from there and stored; later navigations within the app have
// no query string but still need it. Stripping it from the visible URL keeps
// it out of screenshots and shoulder-surfing.
export function writeToken() {
  const p = new URLSearchParams(location.search);
  const k = p.get("k");
  if (k) {
    localStorage.setItem("elanora.token", k);
    p.delete("k");
    const rest = p.toString();
    history.replaceState({}, "", location.pathname + (rest ? "?" + rest : ""));
    return k;
  }
  return localStorage.getItem("elanora.token") || "";
}

export function authHeaders() {
  const t = writeToken();
  return t ? { "X-Elanora-Token": t } : {};
}

export class UploadQueue {
  #db = null;
  #busy = false;

  constructor(base = "") { this.base = base; }

  async #open() {
    if (!this.#db) this.#db = await openDb();
    return this.#db;
  }

  async enqueue(meta, sections) {
    const db = await this.#open();
    await tx(db, "readwrite", (s) =>
      s.put({ trial_id: meta.trial_id, meta, sections, at: Date.now() }));
    // Fire and forget: a failed flush leaves the record in place for the next
    // attempt, so the round is never lost by not awaiting this.
    this.flush().catch(() => {});
  }

  // Everything still queued, as one text file.
  //
  // The queue used to have exactly one way out -- POST to the origin the page
  // was loaded from. When that origin went away, eight recorded rounds went
  // with it: browser storage is scoped per origin, so a new tunnel hostname
  // gets an empty database and the old one can no longer be loaded to reach
  // its contents. A second exit that needs no server at all is the difference
  // between a bad afternoon and lost data.
  async exportAll() {
    const db = await this.#open();
    const all = await tx(db, "readonly", (s) => s.getAll());
    if (!all.length) return { count: 0, blob: null };

    // The same four CSVs the server would have written, at the same paths, so
    // unzipping into data/datasets/ gives a dataset indistinguishable from one
    // that uploaded normally. The old export was a single envelope-format .txt
    // -- re-importable, but not readable by a person, which is half the point
    // of having a rescue file at all.
    const rowsIn = (csv) => (csv ? csv.split("\n").filter(Boolean).length - 1 : 0);
    const entries = [];
    const trialRows = [];

    for (const rec of all) {
      const m = rec.meta;
      const dir = `raw/${m.session_id}`;
      for (const key of ["eeg", "ppg", "imu", "markers"]) {
        const csv = rec.sections[key];
        if (csv) entries.push({ name: `${dir}/${m.trial_id}_${key}.csv`, text: csv });
      }
      // Counted from the payload rather than taken on trust, matching what the
      // server does when it writes this row itself.
      trialRows.push([
        m.trial_id, m.session_id, m.subject_id, m.round_index, m.condition,
        m.frequency_hz, m.jitter_mean_hz, "",
        rowsIn(rec.sections.eeg), rowsIn(rec.sections.ppg), rowsIn(rec.sections.imu),
        m.suspect ?? "0", m.battery_pct ?? "", m.temperature_c ?? "",
      ].join(","));
    }

    entries.push({
      name: "trials.csv",
      text: "trial_id,session_id,subject_id,round_index,condition,frequency_hz," +
            "jitter_mean_hz,started_at,n_eeg,n_ppg,n_imu,suspect,battery_pct," +
            "temperature_c\n" + trialRows.join("\n") + "\n",
    });

    return {
      count: all.length,
      blob: makeZip(entries),
      name: `elanora-rounds-${all.length}.zip`,
    };
  }

  async pending() {
    const db = await this.#open();
    return await tx(db, "readonly", (s) => s.count());
  }

  async flush() {
    if (this.#busy) return;
    this.#busy = true;
    try {
      const db = await this.#open();
      const all = await tx(db, "readonly", (s) => s.getAll());
      for (const rec of all) {
        const res = await fetch(`${this.base}/round`, {
          method: "POST",
          headers: { "Content-Type": "text/plain", ...authHeaders() },
          body: envelope(rec.meta, rec.sections),
        });
        if (res.ok) {
          await tx(db, "readwrite", (s) => s.delete(rec.trial_id));
          continue;
        }
        const text = await res.text();
        // A 401 means the token is missing or wrong. That is fixable by
        // reloading from the QR, and the round must survive until it is -- so
        // it stays queued and the loop stops rather than discarding data.
        if (res.status === 401) {
          this.onRejected?.(rec.trial_id, "write token rejected — reopen the app from the QR code");
          throw new Error("unauthorized");
        }
        // A 400 is the server refusing this specific round -- a bad header, a
        // duplicate. Retrying forever would block every round behind it, so
        // drop it from the queue and surface the reason instead.
        if (res.status === 400) {
          await tx(db, "readwrite", (s) => s.delete(rec.trial_id));
          this.onRejected?.(rec.trial_id, text);
          continue;
        }
        // Anything else is probably transient. Stop and retry later, in order.
        throw new Error(`upload failed: ${res.status} ${text}`);
      }
    } finally {
      this.#busy = false;
    }
  }
}

// ---------------------------------------------------------------------------
// Store-only ZIP
// ---------------------------------------------------------------------------
//
// A rescued round used to come out as one envelope-format .txt, which the
// server could re-import but a person could not read. These are the same four
// CSVs the server would have written, with the same names, so unzipping into
// data/datasets/ gives a dataset indistinguishable from an uploaded one.
//
// Stored rather than deflated: CSV compresses well, but a correct deflate is a
// lot of code to get wrong, and a rescue path is the worst place for a subtle
// bug. Size is not the constraint here -- the data existing at all is.

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function u16(v) { return [v & 0xff, (v >>> 8) & 0xff]; }
function u32(v) { return [v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff]; }

export function makeZip(entries) {
  const enc = new TextEncoder();
  const chunks = [];
  const central = [];
  let offset = 0;

  for (const { name, text } of entries) {
    const nameBytes = enc.encode(name);
    const data = enc.encode(text);
    const crc = crc32(data);

    // Local file header. Timestamps are left at zero: a rescue file's value is
    // its contents, and a wrong mtime is worse than an obviously absent one.
    const local = [
      ...u32(0x04034b50), ...u16(20), ...u16(0), ...u16(0),
      ...u16(0), ...u16(0),
      ...u32(crc), ...u32(data.length), ...u32(data.length),
      ...u16(nameBytes.length), ...u16(0),
    ];
    chunks.push(new Uint8Array(local), nameBytes, data);

    central.push([
      ...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(0), ...u16(0),
      ...u16(0), ...u16(0),
      ...u32(crc), ...u32(data.length), ...u32(data.length),
      ...u16(nameBytes.length), ...u16(0), ...u16(0),
      ...u16(0), ...u16(0), ...u32(0), ...u32(offset),
    ]);
    central.push(nameBytes);
    offset += local.length + nameBytes.length + data.length;
  }

  const dirStart = offset;
  let dirSize = 0;
  for (const c of central) {
    const arr = c instanceof Uint8Array ? c : new Uint8Array(c);
    chunks.push(arr);
    dirSize += arr.length;
  }
  chunks.push(new Uint8Array([
    ...u32(0x06054b50), ...u16(0), ...u16(0),
    ...u16(entries.length), ...u16(entries.length),
    ...u32(dirSize), ...u32(dirStart), ...u16(0),
  ]));

  return new Blob(chunks, { type: "application/zip" });
}
