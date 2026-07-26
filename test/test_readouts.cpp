#include <cstdio>
#include <cmath>
#include <string>
#include "readouts.h"
#include "app_types.h"
#include "../src/vehicle_active.h"

extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;
  check(READOUT_COUNT == (int)StatId::COUNT, "count matches StatId");
  check(READOUT_COUNT == 33, "READOUT_COUNT == 33 (31 + OilP + Gear helper)");
  check(IDX_BARO == (int)StatId::Baro, "IDX_BARO");

  bool seen[8][4] = {{false}};
  int displayed = 0;
  for (int p = 0; p < readoutPageCount(); p++)
    for (int c = 0; c < 4; c++) {
      int idx = readoutAt(p, c);
      if (idx < 0) continue;
      displayed++;
      check(!seen[p][c], "cell holds one stat");
      seen[p][c] = true;
      check(isDisplayed(idx), "readoutAt idx isDisplayed");
    }
  check(displayed == 27, "27 displayed readouts");
  check(readoutPageCount() == 7, "7 pages");

  // Situation-page anchors (full placement map lives in test_layout).
  check(readoutAt(0,0) == (int)StatId::Trans, "p0c0 TRANS");
  check(readoutAt(0,3) == (int)StatId::Egt,   "p0c3 EGT");
  check(readoutAt(2,2) == (int)StatId::Nox,   "p2c2 NOx");
  check(readoutAt(5,0) == (int)StatId::Maf,   "p5c0 MAF");

  // Enhanced tiles are queried @7E0 (header 2). Ambient/Pedal are deactivated
  // (dropped from PAGES, never polled) but their table rows keep the 7E0 defs.
  const StatId q6[] = {StatId::Maf, StatId::Ambient, StatId::Egr, StatId::Pedal, StatId::Cac, StatId::Nox};
  for (StatId s : q6) {
    check(READOUTS[(int)s].cmd != nullptr, "enhanced tile queried");
    check(READOUTS[(int)s].header == 2, "enhanced tile @7E0");
  }

  // Decode sanity.
  float vals[STAT_COUNT] = {0};   // constexpr size (==READOUT_COUNT); READOUT_COUNT is runtime -> VLA on clang
  DecodeCtx ctx{vals};
  uint8_t maf[]  = {0x07,0xD2};   // ((0x07D2))/100 = 20.02 -> rounds 20
  check(std::lround(READOUTS[(int)StatId::Maf].decode(maf,2,ctx)) == 20, "MAF -> 20 g/s");
  uint8_t amb[]  = {0x3A};        // 58-40 = 18C -> 64F
  check(std::lround(READOUTS[(int)StatId::Ambient].decode(amb,1,ctx)) == 64, "AMBIENT 0x3A -> 64F");
  uint8_t egr[]  = {0x3D};        // 61*100/255 = 23.9 -> 24
  check(std::lround(READOUTS[(int)StatId::Egr].decode(egr,1,ctx)) == 24, "EGR 0x3D -> 24%");
  uint8_t ped[]  = {0x23};        // 35*100/255 = 13.7 -> 14
  check(std::lround(READOUTS[(int)StatId::Pedal].decode(ped,1,ctx)) == 14, "PEDAL 0x23 -> 14%");
  uint8_t cac[]  = {0x03,0x56};   // mask d[0]=03, d[1]=0x56=86-40=46C -> 115F
  check(std::lround(READOUTS[(int)StatId::Cac].decode(cac,2,ctx)) == 115, "CAC d[1]=0x56 -> 115F");
  uint8_t nox[]  = {0xC3,0x00,0x93}; // d[1],d[2]=0x0093 = 147 ppm
  check(std::lround(READOUTS[(int)StatId::Nox].decode(nox,3,ctx)) == 147, "NOx -> 147 ppm");
  { uint8_t nff[] = {0xC3,0xFF,0xFF}; // sensor not ready -> NaN (tile shows "--", not 65535)
    float v = READOUTS[(int)StatId::Nox].decode(nff,3,ctx); check(v != v, "NOx 0xFFFF -> NaN"); }

  // --- Frame-validation hardening: bad/sentinel/short frames -> NaN (poll keeps last good) ---
  { uint8_t t[]={0xFF};      float v=READOUTS[(int)StatId::Coolant].decode(t,1,ctx); check(v!=v, "temp 0xFF sentinel -> NaN"); }
  { uint8_t t[]={0x64};      check(std::lround(READOUTS[(int)StatId::Coolant].decode(t,1,ctx))==140, "temp 0x64 -> 140F (valid)"); }
  { uint8_t z[]={0x00,0x00}; float v=READOUTS[(int)StatId::Volts].decode(z,2,ctx);   check(v!=v, "volts 0V -> NaN"); }
  { uint8_t f[]={0xFF,0xFF}; float v=READOUTS[(int)StatId::Volts].decode(f,2,ctx);   check(v!=v, "volts 65V -> NaN"); }
  { uint8_t b[]={0x00};      float v=READOUTS[(int)StatId::Baro].decode(b,1,ctx);    check(v!=v, "baro 0kPa -> NaN"); }
  { uint8_t b[]={0x65};      check(std::lround(READOUTS[(int)StatId::Baro].decode(b,1,ctx))==101, "baro 0x65 -> 101kPa (valid)"); }
  { uint8_t r[]={0x12};      float v=READOUTS[(int)StatId::Rail].decode(r,1,ctx);    check(v!=v, "rail short frame -> NaN"); }
  { uint8_t e[]={0x07};      float v=READOUTS[(int)StatId::Egt].decode(e,1,ctx);     check(v!=v, "egt short frame -> NaN"); }
  // DEF tile reads tank LEVEL from byte[3] (byte[1] is concentration ~32.5%).
  uint8_t def4[] = {0x0E,0x82,0x41,0xE1}; // full tank: [3]=0xE1=225 -> 225/2.55 = 88%
  check(std::lround(READOUTS[(int)StatId::Def].decode(def4,4,ctx)) == 88, "DEF level from byte[3] -> 88%");
  { uint8_t s[] = {0x0E,0x82};            // short frame -> NaN (keep last good, no false 0% alarm)
    float v = READOUTS[(int)StatId::Def].decode(s,2,ctx); check(v != v, "DEF short frame -> NaN"); }
  { uint8_t z[] = {0x0E,0x82,0x41,0x00};  // zero level byte -> NaN (corrupt-frame guard)
    float v = READOUTS[(int)StatId::Def].decode(z,4,ctx); check(v != v, "DEF zero level -> NaN"); }

  // Spot-check a couple existing rows survived (StatId-indexed, layout-independent).
  uint8_t volts[] = {0x36,0x76};
  check(std::fabs(READOUTS[(int)StatId::Volts].decode(volts,2,ctx) - 13.942f) < 0.01f, "volts 13.94");
  check(READOUTS[(int)StatId::Def].thr.warnLo == 10.0f, "DEF warnLo 10");
  check(READOUTS[(int)StatId::Hp].cmd == nullptr, "HP computed");
  check(!isDisplayed(IDX_BARO) && isActive(IDX_BARO), "BARO hidden helper (still polled for boost)");
  check(!isDisplayed((int)StatId::RefTq), "RefTq hidden");

  // --- Units batch: quantity tags + display conversion ---
  // Exactly the 7 temps + speed are tagged; everything else is None.
  const StatId temps[] = {StatId::Trans, StatId::Oil, StatId::Coolant, StatId::Intake,
                          StatId::Egt, StatId::Ambient, StatId::Cac};
  for (StatId s : temps) check(READOUTS[(int)s].quantity == Quantity::Temp, "temp tagged");
  check(READOUTS[(int)StatId::Speed].quantity == Quantity::Speed, "speed tagged");
  check(READOUTS[(int)StatId::Boost].quantity == Quantity::Press, "boost tagged psi->kPa");
  check(READOUTS[(int)StatId::Rpm].quantity   == Quantity::None, "rpm untagged");

  // Imperial passthrough.
  check(toDisplayValue(Quantity::Temp, 212.0f, false) == 212.0f, "temp imperial passthrough");
  check(toDisplayValue(Quantity::Speed, 60.0f, false) == 60.0f, "speed imperial passthrough");

  // Metric conversions.
  check(std::lround(toDisplayValue(Quantity::Temp, 212.0f, true)) == 100, "212F -> 100C");
  check(std::lround(toDisplayValue(Quantity::Temp,  32.0f, true)) == 0,   "32F -> 0C");
  check(std::lround(toDisplayValue(Quantity::Speed, 60.0f, true)) == 97,  "60mph -> 97km/h");
  check(toDisplayValue(Quantity::None, 123.0f, true) == 123.0f, "None: never converts");
  check(std::lround(toDisplayValue(Quantity::Press,   42.0f,   true)) == 290,  "42psi -> 290kPa");
  check(std::lround(toDisplayValue(Quantity::PressHi, 9771.0f, true)) == 67,   "9771psi -> 67MPa");
  check(std::lround(toDisplayValue(Quantity::Vol,     10.0f,   true)) == 38,   "10gal -> 38L");
  check(std::lround(toDisplayValue(Quantity::Flow,    2.4f,    true)  * 10) == 91, "2.4gph -> 9.1L/h");
  // Economy tiles must NOT convert — TRIP page shows both systems as tiles.
  check(READOUTS[(int)StatId::MpgInst].quantity  == Quantity::None, "MPG fixed");
  check(READOUTS[(int)StatId::MpgAvg].quantity   == Quantity::None, "MPG AVG fixed");
  check(READOUTS[(int)StatId::Gal100mi].quantity == Quantity::None, "GAL/100 fixed");
  check(READOUTS[(int)StatId::L100km].quantity   == Quantity::None, "L/100km fixed");
  check(READOUTS[(int)StatId::DpfDp].quantity    == Quantity::None, "DPF dP kPa-native both modes");

  // Unit labels swap only when metric AND tagged.
  check(std::string(displayUnit(READOUTS[(int)StatId::Coolant], false)) == "\xC2\xB0""F", "coolant F");
  check(std::string(displayUnit(READOUTS[(int)StatId::Coolant], true))  == "\xC2\xB0""C", "coolant C");
  check(std::string(displayUnit(READOUTS[(int)StatId::Speed], true))    == "km/h", "speed km/h");
  check(std::string(displayUnit(READOUTS[(int)StatId::Boost], true))    == "kPa",  "boost metric kPa");
  check(std::string(displayUnit(READOUTS[(int)StatId::Rail], true))     == "MPa",  "rail metric MPa");
  check(std::string(displayUnit(READOUTS[(int)StatId::DslFill], true))  == "L",    "DSL+ metric L");
  check(std::string(displayUnit(READOUTS[(int)StatId::FuelRate], true)) == "L/h",  "fuel metric L/h");
  check(std::string(displayUnit(READOUTS[(int)StatId::OilP], true))     == "kPa",  "oil P metric kPa");
  check(std::string(displayUnit(READOUTS[(int)StatId::Gal100mi], true)) == "",     "GAL/100 unit fixed");

  // Alarm zone is unaffected by display unit (computed from canonical value).
  check(zoneFor(240.0f, READOUTS[(int)StatId::Trans].thr) == Zone::Amber, "trans 240F amber regardless of unit");

  // Bar full-scale must cover the critical threshold: a fullScale below critHi
  // pegs the bar at 100% while the number is still green (DPF dP shipped with
  // fullScale 20 vs crit 45 — the bar maxed out in the normal range).
  for (int i = 0; i < READOUT_COUNT; i++) {
    float ch = READOUTS[i].thr.critHi;
    if (ch == ch)   // NaN disables the bound
      check(READOUTS[i].fullScale >= ch, "fullScale covers critHi");
  }

  // --- zoneForStat + lowArmTick: OIL P low alarm arm-delayed on sustained RPM ---
  {
    GaugeSet gs{};
    auto st=[&](StatId id,float v,bool valid=true){ gs.g[(int)id].value=v; gs.g[(int)id].valid=valid; };
    st(StatId::OilP, 0.0f);

    lowArmReset();
    check(zoneForStat(gs,(int)StatId::OilP) == Zone::Green, "key-on 0psi, not armed -> Green");
    lowArmTick(true, 1000);                                  // running streak begins
    lowArmTick(true, 1000 + LOWARM_MS - 1);
    check(!lowAlarmArmed(), "not armed 1ms before the window");
    check(zoneForStat(gs,(int)StatId::OilP) == Zone::Green, "restart ramp 0psi -> still Green");
    lowArmTick(true, 1000 + LOWARM_MS);
    check(lowAlarmArmed(), "armed after sustained running");
    check(zoneForStat(gs,(int)StatId::OilP) == Zone::Red,   "0psi while armed -> Red (real loss)");
    st(StatId::OilP, 10.0f);
    check(zoneForStat(gs,(int)StatId::OilP) == Zone::Amber, "10psi armed -> Amber (warnLo 12)");
    st(StatId::OilP, 20.0f);
    check(zoneForStat(gs,(int)StatId::OilP) == Zone::Green, "20psi hot idle -> Green");
    lowArmTick(false, 60000);                                // auto-stop: RPM gone
    st(StatId::OilP, 0.0f);
    check(!lowAlarmArmed() && zoneForStat(gs,(int)StatId::OilP) == Zone::Green,
          "auto-stop disarms instantly -> 0psi Green");
    lowArmTick(true, 61000);                                 // restart: streak resets
    lowArmTick(true, 61000 + LOWARM_MS - 1);
    check(!lowAlarmArmed(), "streak restarts from zero after disarm");
    st(StatId::Trans, 250.0f);
    check(zoneForStat(gs,(int)StatId::Trans) == Zone::Amber, "gate only touches OilP (Trans unaffected)");
    lowArmReset();
  }

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
