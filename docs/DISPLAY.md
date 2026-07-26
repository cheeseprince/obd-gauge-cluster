# The display

What you see on the screen: tile pages, the alarm model behind the colours, the focus view, day/night
theming, and the status bar.

> Every screenshot in this repository is the **GMC Sierra Duramax** profile — 7 pages, 27 tiles.
> Other vehicles ship fewer and different pages; see [Vehicles](VEHICLES.md).

## Tile inventory

Each vehicle profile defines its own set of pages, with up to four tiles per page; some pages
have an empty cell. GM first, since it's the profile in every screenshot, then BMW, Audi, and
Generic.

### GM Sierra LZ0 — 7 pages, 27 tiles

`src/vehicles/gm_sierra_lz0.cpp:158`

| Page | Tile | What it means |
| :--- | :--- | :--- |
| 1 TOW | Trans | Transmission temperature |
| 1 TOW | Coolant | Coolant temperature |
| 1 TOW | OilP | Oil pressure |
| 1 TOW | Egt | Exhaust gas temperature |
| 2 POWER | Boost | Turbo boost |
| 2 POWER | Hp | Horsepower |
| 2 POWER | Rpm | Engine speed |
| 2 POWER | Load | Engine load |
| 3 REGEN | DpfDp | DPF differential pressure |
| 3 REGEN | FuelRate | Fuel rate |
| 3 REGEN | Nox | NOx |
| 3 REGEN | Rail | Fuel rail pressure |
| 4 RANGE | FuelLevel | Fuel level |
| 4 RANGE | DslFill | Diesel fill level |
| 4 RANGE | Def | DEF level |
| 4 RANGE | DefFill | DEF fill level |
| 5 TRIP | MpgInst | Instantaneous MPG |
| 5 TRIP | MpgAvg | Average MPG |
| 5 TRIP | Gal100mi | Gallons per 100 miles |
| 5 TRIP | L100km | Litres per 100 km |
| 6 DIAG | Maf | Mass air flow |
| 6 DIAG | Egr | EGR |
| 6 DIAG | Cac | Charge air cooler temp |
| 6 DIAG | Intake | Intake air temperature |
| 7 MISC | Speed | Vehicle speed |
| 7 MISC | Volts | Battery voltage |
| 7 MISC | Oil | Oil temperature |
| 7 MISC | *(empty)* | Page 7's fourth cell is unused — this is the empty cell, not a bug |

### BMW F10 535i — 3 pages, 11 tiles

`src/vehicles/bmw_f10_535i.cpp:147`

| Page | Tile | What it means |
| :--- | :--- | :--- |
| 1 ENGINE | Coolant | Coolant temperature |
| 1 ENGINE | OilP | Oil pressure |
| 1 ENGINE | Boost | Turbo boost |
| 1 ENGINE | Load | Engine load |
| 2 DRIVE | Rpm | Engine speed |
| 2 DRIVE | Speed | Vehicle speed |
| 2 DRIVE | Intake | Intake air temperature |
| 2 DRIVE | Ambient | Ambient air temperature |
| 3 MISC | Baro | Barometric pressure |
| 3 MISC | FuelLevel | Fuel level |
| 3 MISC | Volts | Battery voltage |
| 3 MISC | *(empty)* | Page 3's fourth cell is unused — BMW also has an empty cell, 11 tiles not 12 |

### Audi Q5 — 3 pages, 12 tiles

`src/vehicles/audi_q5.cpp:139`

| Page | Tile | What it means |
| :--- | :--- | :--- |
| 1 TEMPS | Trans | Transmission temperature |
| 1 TEMPS | Oil | Oil temperature |
| 1 TEMPS | Coolant | Coolant temperature |
| 1 TEMPS | Boost | Turbo boost |
| 2 DRIVE | Rpm | Engine speed |
| 2 DRIVE | Speed | Vehicle speed |
| 2 DRIVE | Load | Engine load |
| 2 DRIVE | Volts | Battery voltage |
| 3 AIR | Intake | Intake air temperature |
| 3 AIR | Pedal | Accelerator pedal position |
| 3 AIR | Maf | Mass air flow |
| 3 AIR | FuelLevel | Fuel level |

### Generic OBD — 2 pages, 8 tiles

`src/vehicles/generic_obd.cpp:81`

| Page | Tile | What it means |
| :--- | :--- | :--- |
| 1 ENGINE | Rpm | Engine speed |
| 1 ENGINE | Speed | Vehicle speed |
| 1 ENGINE | Coolant | Coolant temperature |
| 1 ENGINE | Load | Engine load |
| 2 AIR | Intake | Intake air temperature |
| 2 AIR | Pedal | Accelerator pedal position |
| 2 AIR | Maf | Mass air flow |
| 2 AIR | Volts | Battery voltage |

Generic OBD also polls `FuelLevel` (PID `012F`) live, but both of its pages are already full, so
it's scheduled as a helper value rather than shown as its own tile
(`generic_obd.cpp:82-84`).

## The alarm model

Warn and critical bands are **per-stat and per-vehicle** — they come from each profile's
`Thresholds` struct (warn/crit high and low), not from a single global table
(`src/gauge_model.h:10-16`, `zoneFor()`).

An alarm only fires after the stat has been continuously non-Green for **4000 ms**; it resets
back to armed the moment the stat returns to Green. This hold-off is tracked per stat index, so
one tile's alarm state doesn't affect another's (`src/alarm_holdoff.h:11`,
`alarm_holdoff.cpp:3-7`).

**Oil pressure gets special arming logic.** The low-pressure alarm only arms once RPM has been
**≥ 400** for a sustained **20 000 ms** of confirmed engine-running, and it disarms instantly if
RPM drops, goes invalid, or the OBD link drops (`src/gauge_model.h:23-24`,
`gauge_model.cpp:36-42`). RPM has to be fresh within the last **4000 ms** to count toward that —
a held/stale value doesn't arm it (`src/gauge_model.h:31`).

This exists because gating the same logic on fuel flow instead of RPM failed on the actual truck:
FUEL is a slow-tier PID (~35 s cadence), so its last-good value stayed stale through exactly the
transitions that mattered. A 2026-07-17 log replay of that gating produced **11 false fires** at
auto-stop shutdowns and key-on windows. RPM is fast-tier (~2 s), so arming on sustained RPM
instead removed all of them (`gauge_model.cpp:21-31`).

Stale values — a held value from a PID that's stopped responding — are shown in grey and are
excluded from alarming entirely (`src/ui.cpp:700`).

## Focus view

Press a tile to zoom into it. Focus view shows a rolling trend graph for that stat, with each
sample coloured by the alarm zone it was in when it was recorded.

The trend window is **5 minutes**: a 300-sample ring at 1 Hz (`HISTORY_LEN`, `src/history.h:11,13`).

## Theming

The display switches between day and night themes based on solar position combined with the
on-board clock. Brightness and imperial/metric units are also configurable.

## Status bar

| Element | States |
| :--- | :--- |
| Bluetooth icon | Blue (connected) / grey (not connected) |
| Clock | Current time |
| Page indicator | Shows which of the profile's pages is active |
| Log status | Green `LOG` (logging normally) · red `LOG` (write error) · grey `LOG` (logging off) · grey `No SD Card` (no card present) |

## Rearranging tiles

Tile layout and page assignment are configurable — see
[Customizing the views](CUSTOMIZING-VIEWS.md).
