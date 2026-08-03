# Roadmap

<!-- SANITISATION RULE — read this before editing.
     This file is public. It must never name an unimplemented protection, an
     unenforced guarantee, or a gap in CI or test coverage. Security work is
     tracked privately; SECURITY.md is the only security-facing document in
     this repo. Features and vehicle support only. -->

Horizons, not dates — this is a spare-time project and dated milestones would
only age badly. Vehicle support moves when a car is available to scan.

## What's coming

### Recently shipped
- **BMW 535i (F10, N55)** — scanned on-car; the profile is in use and running
  without reported errors. Not validated to the depth of the Sierra profile.
  See [`docs/BMW-STATUS.md`](docs/BMW-STATUS.md).
- **Per-task stack headroom on the VERSION card** — the OTA/TLS path's
  remaining stack is now a measured number rather than an estimate.

### Now
- **Ford 6.7L Power Stroke (2017–22 Super Duty)** — researched, awaiting an
  on-car scan. See [`docs/FORD-STATUS.md`](docs/FORD-STATUS.md).

### Next
- **Promote the Audi Q5 and Jeep Wagoneer skeletons** to validated profiles.
  Both need seat time in the car; the PID maps are drafted.

### Later / maybe
- Additional vehicle profiles, driven by which cars are actually reachable.
- Further dash pages for non-diesel platforms, where the ECU reports enough
  to justify one.

### Not planned
- **GPS / location logging.** Deliberate, not an oversight. A GPS would mean
  the device recording where its owner drives, and nothing here needs that:
  the clock is set from NTP during an update check, and the location behind
  local time and sunrise/sunset is a lat/long you provision once in the
  settings menu. The device never senses where it is — it only knows what
  you told it.

## How to help

| Area | Good first issue? | Where to start |
| :--- | :--- | :--- |
| Add or extend a vehicle profile | **Yes** | [`docs/VEHICLES.md`](docs/VEHICLES.md), then a `docs/<MAKE>-STATUS.md` |
| Scan a car we have no data for | **Yes** — needs the car, not deep code | `tools/obd_scan/` |
| Hardware-in-the-loop test rig | No — needs the board | `tools/hil/` |
| Documentation and READMEs | **Yes** | Anything in [`docs/`](docs/) |

The most useful contribution is **data from a car we cannot reach.** A profile
is mostly a PID map, and the map comes from a scan.
