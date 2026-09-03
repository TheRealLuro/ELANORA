// Muse 2 over Web Bluetooth.
//
// This layer reports packets and nothing more. It deliberately does not invent
// timestamps -- the headset sends a 16-bit sequence number and no clock, and
// turning sequences into a timebase is ringbuffer.js's job. Keeping the two
// apart is what lets the decode be tested against captured bytes.
//
// The identifiers below were confirmed against a real headset with
// inspect.html before anything was built on them.

export const MUSE_SERVICE = "0000fe8d-0000-1000-8000-00805f9b34fb";

const S = (n) => `273e${n}-4c4d-454d-96be-f03bac821358`;

export const CHAR = {
  control: S("0001"),
  TP9: S("0003"),
  AF7: S("0004"),
  AF8: S("0005"),
  TP10: S("0006"),
  gyro: S("0009"),
  accel: S("000a"),
  telemetry: S("000b"),
  ppgAmbient: S("000f"),
  ppgIr: S("0010"),
  ppgRed: S("0011"),
};

export const EEG_NAMES = ["TP9", "AF7", "AF8", "TP10"];

// Commands go out length-prefixed with a trailing newline: 0x02 'd' '\n'.
function command(text) {
  const bytes = [text.length + 1];
  for (const ch of text) bytes.push(ch.charCodeAt(0));
  bytes.push(10);
  return new Uint8Array(bytes);
}

export class Muse {
  #device = null;
  #chars = new Map();
  #rawCb = null;
  #lostCb = null;

  get connected() { return this.#device?.gatt?.connected === true; }
  get name() { return this.#device?.name ?? ""; }
  get streams() { return [...this.#chars.keys()].filter((k) => k !== "control"); }

  onRaw(cb) { this.#rawCb = cb; }
  onLost(cb) { this.#lostCb = cb; }

  async connect() {
    if (!("bluetooth" in navigator)) {
      // Two very different problems with the same symptom, and the fix differs,
      // so name which one it is rather than saying "connection failed".
      throw new Error(
        window.isSecureContext
          ? "This browser has no Web Bluetooth. On iOS, Safari never has — open this page in Bluefy."
          : "Not a secure context, so Web Bluetooth is withheld. The page must be served over HTTPS."
      );
    }

    this.#device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [MUSE_SERVICE] }],
      optionalServices: [MUSE_SERVICE],
    });
    this.#device.addEventListener("gattserverdisconnected", () => {
      this.#lostCb?.();
    });

    const server = await this.#device.gatt.connect();
    const service = await server.getPrimaryService(MUSE_SERVICE);

    for (const [name, uuid] of Object.entries(CHAR)) {
      try {
        this.#chars.set(name, await service.getCharacteristic(uuid));
      } catch {
        // A characteristic the headset does not expose is not an error --
        // the 2016 model has no PPG. The caller decides what it needs.
      }
    }
    if (!this.#chars.has("control")) {
      throw new Error("connected, but the control characteristic is missing");
    }
    const missing = EEG_NAMES.filter((n) => !this.#chars.has(n));
    if (missing.length) {
      throw new Error(`missing EEG channels: ${missing.join(", ")}`);
    }
  }

  async #write(text) {
    const ch = this.#chars.get("control");
    // writeValueWithoutResponse is what the headset expects; the with-response
    // form times out on some stacks.
    await ch.writeValueWithoutResponse(command(text));
  }

  async start() {
    // p50 enables PPG. Sent before "d" because a preset change mid-stream is
    // ignored, and the failure mode is a silent absence of PPG rather than an
    // error -- which would only surface as an untrained heart model, weeks later.
    await this.#write("p50");

    for (const [name, ch] of this.#chars) {
      if (name === "control") continue;
      await ch.startNotifications();
      ch.addEventListener("characteristicvaluechanged", (e) => {
        this.#rawCb?.(name, e.target.value);
      });
    }
    await this.#write("d");
  }

  async stop() {
    try { await this.#write("h"); } catch { /* already gone */ }
    if (this.#device?.gatt?.connected) this.#device.gatt.disconnect();
  }
}
