# Customizing the views

The screen is a set of pages, each with four tiles. Which tiles appear, in what order, on
how many pages, is defined by one table in the vehicle profile — edit it, rebuild, flash.

## Where the layout lives

Each vehicle profile is a file under [`src/vehicles/`](../src/vehicles/). The GM profile is
`src/vehicles/gm_sierra_lz0.cpp`. Near the bottom it has the layout:

```cpp
#define _ StatId::COUNT
static const StatId PAGES[][4] = {
  { StatId::Trans,   StatId::Coolant, StatId::OilP,     StatId::Egt     }, // TOW
  { StatId::Boost,   StatId::Hp,      StatId::Rpm,      StatId::Load    }, // POWER
  { StatId::DpfDp,   StatId::FuelRate, StatId::Nox,     StatId::Rail    }, // REGEN
  { StatId::FuelLevel, StatId::DslFill, StatId::Def,    StatId::DefFill }, // RANGE
  { StatId::MpgInst, StatId::MpgAvg,  StatId::Gal100mi, StatId::L100km  }, // TRIP
  { StatId::Maf,     StatId::Egr,     StatId::Cac,      StatId::Intake  }, // DIAG
  { StatId::Speed,   StatId::Volts,   StatId::Oil,      _               }, // MISC
};
#undef _

static const char* const PAGE_NAMES[] = { "TOW","POWER","REGEN","RANGE","TRIP","DIAG","MISC" };
```

- **Each row is one page** of exactly four tiles.
- **`_` is an empty cell** (the MISC page above has three tiles).
- **Order = display order.** The knob scrolls tiles in reading order and wraps around, so
  the last page is one detent back from the first.
- **`PAGE_NAMES` runs parallel to `PAGES`** — one short name per page, shown in the header.
  There must be exactly one name per page (a `static_assert` enforces it).

## Common edits

**Reorder tiles or pages** — move entries around. To put oil temperature on the TOW page,
swap it in:

```cpp
{ StatId::Trans, StatId::Coolant, StatId::OilP, StatId::Oil }, // TOW
```

**Add or remove a page** — add or delete a row (and its `PAGE_NAMES` entry). Any number of
pages works.

**Hide a tile but keep polling it** — move it to the `HELPERS` list instead of `PAGES`:

```cpp
static const StatId HELPERS[] = { StatId::RefTq, StatId::Baro, StatId::ActTq, StatId::Gear };
```

`HELPERS` are read from the vehicle but never drawn. Use this for values a computed tile
needs (e.g. barometric pressure feeds the boost calculation) or data you only want in the
SD log (gear, for towing shift analysis).

**Deactivate a tile entirely** — remove it from **both** `PAGES` and `HELPERS`. It is then
neither shown nor polled, which frees bus time.

## Preview without hardware

Before flashing, render the exact pixels the panel will show:

```
cd tools/ui_snapshot && make snapshot && ./snapshot
```

This compiles the real UI code and writes one PNG per page (day, night, and metric
variants). Check your layout there, then build and flash:

```
pio run -e crowpanel_obd -t upload
```

## Adding a whole new vehicle

A new vehicle is a new profile file, not just a layout change — it needs the PID table,
decoders, thresholds and tank sizes for that vehicle, which must be measured on the vehicle
first. See [`PORTING-LESSONS.md`](PORTING-LESSONS.md).
