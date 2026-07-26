# Hardware choices

Why this board, this input, and this adapter — and what was tried first.

## Display board: CrowPanel Advance 3.5"

The current board is an **Elecrow CrowPanel Advance 3.5"** (ESP32-S3-WROOM-1-N16R8, IPS
480×320). It replaced an earlier **Elecrow 3.5" WROVER-B** (still supported via the
`elecrow` build envs as a classic-Bluetooth option).

| | Old — WROVER-B | Current — CrowPanel Advance |
| :--- | :--- | :--- |
| Panel | ILI9488 **TN**, ~250–300 nits | **IPS, 400 nits** |
| Daylight readability | Poor — washes out in a sunlit cab | The reason for the change |
| Resolution | 480×320 | 480×320 — same, so **no UI rework** |
| MCU | ESP32 (WROVER) | ESP32-S3 (PSRAM) |
| Encoder port | none broken out | on-board **I²C-OUT (Qwiic)** header |

The IPS panel is the win: a TN display at ~250–300 nits is hard to read through a
windshield's worth of daylight, and an IPS panel at 400 nits with wide viewing angles
solves it. Keeping the same 480×320 resolution meant the entire UI carried over unchanged.
The on-board I²C-OUT header is what makes the rotary-encoder input clean — no GPIO wiring.

**Note on Bluetooth:** the CrowPanel Advance is an ESP32-**S3**, which does **BLE only**.
The dash reads OBD over BLE (a Vgate vLinker MS). The older WROVER board
is a classic ESP32 with Bluetooth Classic SPP — kept as the `elecrow` option for
classic-Bluetooth ELM327 adapters. Pick the board to match the adapter you have.

## Input: rotary encoder (three approaches tried)

Navigation went through three input methods before settling on a rotary encoder.

**1. Resistive touch** — the original all-in-one board's touchscreen. Two problems: the TN
panel it sat on was hard to read in daylight, and the touch itself was unreliable in a
moving vehicle (a resistive panel's dual-sample read rejects a moving finger, and it needed
manual calibration with the axes swapped). Touch navigation was removed from the firmware.

**2. Physical push buttons** — six momentary buttons through a PCF8574 I²C GPIO expander,
mounted in a 3D-printed bezel. This worked well and was reliable, but it meant six buttons,
an expander module, and the hookup wiring behind the panel.

**3. Capacitive touch** — the CrowPanel Advance has a capacitive touch controller. Better
than resistive, but it shares touch's core drawbacks for this use: you have to look at the
screen to aim, and fingerprints/glare fight the daylight-readability goal. Left unused.

**4. Rotary encoder — chosen.** An **Arduino Modulino Knob** (Bourns PEC11J, 30 detents +
push), on the I²C bus. It won because:

- **Blind-operable.** You feel detents without looking — turn to move, press to select —
  which matters while driving.
- **One control.** A single knob does everything (turn / press / hold), so the case and the
  cognitive load are both minimal.
- **Mounts anywhere.** It is cable-connected over I²C, so it can sit beside the screen or
  off to one side, wherever the cable reaches.
- **No GPIO cost.** It rides the existing I²C bus (shared with the RTC and touch
  controller, different address) — a single cable, no expander, no soldering.

See [`WIRING.md`](WIRING.md) for the one cable it needs.

## OBD adapter

The firmware speaks to **BLE** ELM327 adapters on the CrowPanel dash, and
**classic-Bluetooth** ones on the `elecrow_obd` build. There is **no WiFi-OBD transport** in
this repository — a WiFi ELM327 will not connect to the dash. Full compatibility matrix:
[`ADAPTERS.md`](ADAPTERS.md).

- **Vgate vLinker MS (BLE)** — the dash's adapter, and the only one validated on hardware.
  Reads this truck's enhanced GM PIDs with 11-bit headers. Ships in a Classic/MFi-only mode;
  switch it to BT+BLE once with Vgate's updater app so the ESP32-S3 can pair.

Addressing turned out to be **adapter-dependent** on the same vehicle: a WiFi ELM327 used with
the laptop scanner answered only 29-bit addressing on this truck, while the vLinker over BLE
answered 11-bit. Worth knowing if a working adapter reads nothing until the protocol is right —
see [`PORTING-LESSONS.md`](PORTING-LESSONS.md).

## Fasteners and case

The 3D-printed case ([Printables](https://www.printables.com/model/1788789-odb-gauge-cluster-case))
uses **8 × M3 heat-set threaded inserts** (4 mm long, 5 mm OD) and **4 × M3×10 + 4 × M3×6**
screws. BOM and dimensions: [`../hardware/`](../hardware/).
