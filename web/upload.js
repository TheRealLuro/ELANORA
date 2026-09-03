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
          headers: { "Content-Type": "text/plain" },
          body: envelope(rec.meta, rec.sections),
        });
        if (res.ok) {
          await tx(db, "readwrite", (s) => s.delete(rec.trial_id));
          continue;
        }
        const text = await res.text();
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
