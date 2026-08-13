# Customizing the views

The screen is a set of pages, each with four cells. Which tiles appear, in what order, on
how many pages, is defined by one table in the vehicle profile — edit it, rebuild, flash.

## Where the layout lives

Each vehicle profile is a file under [`src/vehicles/`](../src/vehicles/). The GM profile is
`src/vehicles/gm_sierra_lz0.cpp`. Near the bottom it has the layout:

```cpp
#define _ StatId::COUNT
static const StatId PAGES[][4] = {
  { StatId::Trans,   StatId::Coolant, StatId::OilP,     StatId::Egt     }, // TOWING
  { StatId::Boost,   StatId::Hp,      StatId::Rpm,      StatId::Load    }, // POWER
  { StatId::DpfDp,   StatId::FuelRate, StatId::Nox,     StatId::Rail    }, // REGENERATION
  { StatId::FuelLevel, StatId::DslFill, StatId::Def,    StatId::DefFill }, // RANGE
  { StatId::MpgInst, StatId::MpgAvg,  StatId::Gal100mi, StatId::L100km  }, // TRIP
  { StatId::Maf,     StatId::Egr,     StatId::Cac,      StatId::Intake  }, // DIAGNOSTICS
  { StatId::Speed,   StatId::Volts,   StatId::Oil,      _               }, // MISCELLANEOUS
};
#undef _

static const char* const PAGE_NAMES[] = { "TOWING","POWER","REGENERATION","RANGE","TRIP","DIAGNOSTICS","MISCELLANEOUS" };
```

- **Each row is one page** of exactly four *cells* — a cell may be empty, so a page can show
  fewer than four tiles.
- **`_` is an empty cell** (the MISCELLANEOUS page above has three tiles).
- **Order = display order.** The knob scrolls tiles in reading order and wraps around, so
  the last page is one detent back from the first.
- **`PAGE_NAMES` runs parallel to `PAGES`** — one name per page, shown in the header.
  There must be exactly one name per page (a `static_assert` enforces it). Names are drawn
  at montserrat_20 with a 298 px budget (the header also carries the clock and SD state), so
  keep them under roughly 18 characters.
- **Tile labels are not set here.** The text on a tile comes from `STAT_LABELS` in
  `src/readouts.cpp`, keyed by `StatId` and shared by every vehicle, so moving a tile between
  pages carries its label along. `ReadoutDef::name` in the profile is the SD-log CSV column
  key, not the on-screen text — changing it forks the log schema.
- ⚠️ **A `StatId` may appear on exactly ONE page.** Listing the same stat twice does not
  duplicate the tile — it breaks knob navigation, because `readoutPageOf()` returns the
  *first* page holding a stat and `cursorStep()` finds its *first* slot in the reading
  order. The cursor then jumps backwards to the earlier page instead of advancing.
  This shipped once: the Ford `RANGE` page re-listed two stats that already lived on other
  pages, and the dash intermittently went from page 8 to page 4. Use `_` for a spare cell
  rather than padding a page with a stat that lives elsewhere. A test in
  `test/test_vehicle_registry.cpp` now enforces this across every profile, so a duplicate
  fails the build rather than the drive.

## Common edits

**Reorder tiles or pages** — move entries around. To put oil temperature on the TOWING page,
swap it in:

```cpp
{ StatId::Trans, StatId::Coolant, StatId::OilP, StatId::Oil }, // TOWING
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

`HELPERS` and `PAGES` must be **disjoint** — a stat in both is a contradiction about whether
it is displayed, and the same registry test rejects it. Promoting a helper to a page means
deleting it from `HELPERS`.

**Deactivate a tile entirely** — remove it from **both** `PAGES` and `HELPERS`. It is then
neither shown nor polled, which frees bus time.

## Preview without hardware

Before flashing, render the exact pixels the panel will show:

```
cd tools/ui_snapshot && make snapshot && ./snapshot
```

This compiles the real UI code and writes one PPM per page (day, night, and metric
variants) into `tools/ui_snapshot/`. Check your layout there, then build and flash:

```
pio run -e crowpanel_obd -t upload
```

**If your change altered anything on screen, refresh the committed screenshots too** —
CI fails otherwise, because it re-renders and compares them against the code:

```
cd tools/ui_snapshot && make && ./snapshot && python3 render_docs_images.py
```

That converts the PPMs into `docs/images/*.png`. Add `--check` to verify without
writing, which is exactly what CI runs.

⚠️ **Run `make` first, every time.** `./snapshot` executes the existing binary and does not
rebuild it. Against a stale binary it re-renders the *old* UI, which of course still matches
the committed PNGs, and `--check` reports everything current — a false pass that CI then
contradicts, because a fresh runner has no stale objects to reuse.

## Adding a whole new vehicle

A new vehicle is a new profile file, not just a layout change — it needs the PID table,
decoders, thresholds and tank sizes for that vehicle, which must be measured on the vehicle
first. See [`PORTING-LESSONS.md`](PORTING-LESSONS.md).
