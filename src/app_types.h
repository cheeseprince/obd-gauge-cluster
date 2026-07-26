#pragma once
#include "gauge_model.h"

// StatId is the GLOBAL stat vocabulary — the union across all vehicle profiles.
// APPEND-ONLY: never reorder or remove entries; history rings, CSV columns,
// NVS knobstat and alarm arrays are all indexed by these values.
enum class StatId { Trans, Oil, Boost, Coolant, Volts, Intake, Rpm, Speed,
                    Egt, DpfDp, FuelRate, Load, MpgInst, MpgAvg,
                    Gal100mi, L100km, Rail, Hp, Def, FuelLevel, DslFill, DefFill,
                    ActTq, RefTq, Baro,
                    Maf, Ambient, Egr, Pedal, Cac, Nox,
                    // From the 2026-07-17 probe drive: 22115C confirmed as oil
                    // pressure (tile), 22199A as current gear (logged helper).
                    OilP, Gear, COUNT };
inline constexpr int STAT_COUNT = (int)StatId::COUNT;

struct GaugeSet { Gauge g[STAT_COUNT]; };
