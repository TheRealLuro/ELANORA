# Setup

## Prerequisites

A C++20 toolchain and CMake 3.24+. On Windows:

```powershell
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install Kitware.CMake
```

Everything else is fetched at configure time: BrainFlow, Dear ImGui, ImPlot,
GLFW, Eigen, Catch2, cpp-httplib. miniaudio is a single header downloaded on
first configure.

## Build

```bash
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build -C Release
```

A quick toolchain check without the heavy dependencies:

```bash
cmake -B build -S . -DELANORA_BUILD_APPS=OFF -DELANORA_WITH_BRAINFLOW=OFF
```

---

## Recording a session

### 1. Start the server

```bash
./build/bin/Release/elanora_serve --port 8080 --web web --root data/datasets
```

It prints a write token and the machine's LAN address. Writes require that
token; reads do not.

### 2. Reach it from the phone

**iOS needs [Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055).**
Safari has no Web Bluetooth and never has — Apple declined to implement it in
WebKit. Android Chrome works natively.

Open `http://<lan-address>:8080/probe.html` first. It reports whether
`navigator.bluetooth` exists in that browser, which is the one thing everything
else depends on.

**If the browser refuses plain HTTP**, Web Bluetooth needs a secure context.
The least painful route, with no certificate to install on the phone:

```bash
cloudflared tunnel --url http://localhost:8080
```

Append `?k=<token>` to the URL so the page can upload. It stores the token and
strips it from the address bar.

> Uploads then route through Cloudflare. Fine for testing on yourself; decide
> deliberately before collecting from other subjects.

### 3. Before pressing Start

| | Why |
|---|---|
| **Ringer ON** — not the silent switch | iOS gives plain web audio a category that obeys that switch. The app forces the louder category, but the switch is the first thing to check if the test tone is silent. |
| **Run the sound test** | A silenced session still records clean EEG, correct markers and the right round lengths, with no stimulus in any of it. No software can detect this — iOS mutes downstream of anything the page can observe. |
| **Auto-Lock → Never** | Settings → Display & Brightness. A screen lock suspends Bluetooth and audio mid-round. |
| **Headphones on, volume set once** | Changing it mid-session makes loudness differ between conditions. |
| **Focus / Do Not Disturb on** | A notification sound inside a stimulus window is an uncontrolled auditory event. |
| **Close the Muse app** | The headset accepts one Bluetooth connection at a time. |

### 4. Check the electrodes

All four should read `good` at 3–50 µV once the headset settles. Then close
your eyes for ten seconds and watch alpha rise on AF7 and AF8.

If it does not, the seating is wrong, and no amount of analysis will recover
it. This is the gate worth being stubborn about.

### 5. Run it

Press Start. The session runs itself: 18 rounds, the survey opening after each
one. Watch rounds land on the PC at `http://localhost:8080/data.html`.

Stopping early keeps every round that completed the full cycle — baseline,
stimulus, post, questions — and discards only the one in progress.

---

## Analysis

```bash
./build/bin/Release/elanora_data      # features + evidence gate
./build/bin/Release/elanora_models    # train, then invert
```

**Read the Evidence tab before trusting any recommendation.** If every outcome
reads `NoEvidence`, that is a valid result: frequency did not measurably affect
this subject pool under this protocol. The honest next step is a protocol
change, not an optimizer.

Sanity ranges — values outside these mean a DSP bug, not a discovery:

- BPM 50–100
- Breathing 6–25 /min
- Alpha rising when the eyes close

---

## Troubleshooting

**No device in the Bluetooth picker.** The service UUID is wrong for your
headset, or the Muse app still holds the connection. `inspect.html` shows raw
packets and decoded values side by side, which distinguishes the two.

**`navigator.bluetooth` undefined.** Either not a secure context, or not a
browser with Web Bluetooth. The probe page says which.

**Test tone silent.** Ringer switch first, then volume, then headphone
connection.

**Electrodes read flat.** Wet the sensors slightly, and check the headband sits
on skin rather than hair at TP9/TP10.

**Rounds not appearing on the PC.** `data.html` counts files on disk rather
than trial rows, so a row with missing CSVs shows as an error rather than a
success. Check the server console — every request is logged with its peer
address, which distinguishes "never arrived" from "arrived and was rejected".

**Desktop cannot connect.** A Muse 2 needs a Bluetooth radio; its USB port is a
control channel only and carries no sample data. Either a USB Bluetooth adapter
(the existing path then works unchanged) or a BLED112 dongle, which
`muse_monitor` supports via the USB dongle option.
