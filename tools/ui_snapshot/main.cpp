// Host UI snapshot tool: renders the REAL firmware quad UI (ui.cpp + LVGL)
// into PPM images — pixel-exact previews of what the 480x320 panel shows.
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "ui.h"
#include "readouts.h"
#include "gauge_model.h"
#include "settings.h"
#include "settings_menu.h"
#include "sd_log.h"
#include "vehicle_active.h"
extern const VehicleProfile GM_SIERRA_LZ0_PROFILE;

// Log status is a stub here; make it settable so the status-bar previews can
// exercise every state (On=green, Err=red, NoCard="No SD Card", Off=grey).
static LogStatus g_snapLog = LogStatus::On;

static lv_color_t fb[480 * 320];                 // captured framebuffer (RGB565)
static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf1[480 * 320];

static void flushCb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++)
      fb[y * 480 + x] = *px++;
  lv_disp_flush_ready(d);
}

static void dumpPpm(const char* path) {
  FILE* f = fopen(path, "wb");
  fprintf(f, "P6\n480 320\n255\n");
  for (int i = 0; i < 480 * 320; i++) {
    uint16_t c = fb[i].full;
    uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 0x1F) * 255 / 31),
                       (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63),
                       (uint8_t)(( c        & 0x1F) * 255 / 31) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

int main() {
  lv_init();
  lv_disp_draw_buf_init(&drawBuf, buf1, nullptr, 480 * 320);
  static lv_disp_drv_t drv; lv_disp_drv_init(&drv);
  drv.hor_res = 480; drv.ver_res = 320;
  drv.draw_buf = &drawBuf; drv.flush_cb = flushCb;
  lv_disp_drv_register(&drv);

  g_activeProfile = &GM_SIERRA_LZ0_PROFILE;   // render the GM Duramax profile
  ui::begin();
  ui::suppressAlarms(true);        // no alarm overlay in these previews
  // (BT icon starts gray; showStatus(nullptr) below links it blue after the
  //  gray-state capture.)

  // Realistic values straight from the 2026-07-17 drive medians/peaks.
  GaugeSet gs{};
  auto set = [&](StatId id, float v){ gs.g[(int)id].value = v; gs.g[(int)id].peak = v; gs.g[(int)id].valid = true; };
  set(StatId::Trans,126); set(StatId::Coolant,216); set(StatId::OilP,42.5f); set(StatId::Egt,584);
  set(StatId::Boost,8.1f); set(StatId::Hp,184); set(StatId::Rpm,1650); set(StatId::Load,47);
  set(StatId::DpfDp,6.2f); set(StatId::FuelRate,2.4f); set(StatId::Nox,135); set(StatId::Rail,9771);
  set(StatId::FuelLevel,34); set(StatId::DslFill,15.8f); set(StatId::Def,81); set(StatId::DefFill,1.0f);
  set(StatId::MpgInst,24.8f); set(StatId::MpgAvg,22.3f); set(StatId::Gal100mi,4.5f); set(StatId::L100km,10.6f);
  set(StatId::Maf,28); set(StatId::Egr,24); set(StatId::Cac,174); set(StatId::Intake,86);
  set(StatId::Speed,62); set(StatId::Volts,13.6f); set(StatId::Oil,221);

  // Seed a realistic 5-minute trend for every stat so the Focus view renders a
  // populated graph rather than an empty pane. Trans climbs from a cold start
  // toward its current value and settles — the shape a real warm-up produces.
  HistorySet hist{};
  for (int s = 0; s < HISTORY_LEN; s++) {
    float t = (float)s / (float)(HISTORY_LEN - 1);        // 0 -> 1 across the window
    for (int i = 0; i < STAT_COUNT; i++) {
      float now = gs.g[i].value;
      // Warm-up curve for the temperatures, mild wander for everything else.
      float v = (i == (int)StatId::Trans || i == (int)StatId::Coolant ||
                 i == (int)StatId::Oil   || i == (int)StatId::Egt)
                  ? now * (0.72f + 0.28f * (1.0f - expf(-3.2f * t)))
                  : now * (0.94f + 0.06f * sinf(t * 11.0f));
      hist.ring[i].push(v);
    }
  }

  NavState nav{};
  nav.view = View::Quad;
  nav.rtcValid = true;                       // exercise the status-bar clock
  nav.rtcNow = DateTime{2026, 7, 19, 12, 47, 0};

  // Bluetooth icon GRAY (OBD not yet linked): captured before showStatus() links
  // it. At init the connect overlay is hidden and btLabel is gray, so this is the
  // status band the instant before a link comes up. Then link it blue for the rest.
  nav.quadPage = 0; nav.focus = (StatId)readoutAt(0, 0);
  ui::render(gs, nav, Theme::Day, hist, false);
  lv_refr_now(nullptr);
  dumpPpm("statusbar_bt_gray.ppm");
  ui::showStatus(nullptr, Theme::Day);        // OBD linked -> Bluetooth icon blue

  char path[64];
  for (int p = 0; p < readoutPageCount(); p++) {
    nav.quadPage = p;
    nav.focus = (StatId)readoutAt(p, 0);       // encoder cursor on cell 0
    ui::render(gs, nav, Theme::Day, hist, false);
    lv_refr_now(nullptr);
    snprintf(path, sizeof path, "page%d_day.ppm", p);
    dumpPpm(path);
  }
  // --- Status-bar states: BT link icon (blue, linked above) + clock + the
  // four log/SD indicator states, all on the TOW page 0. ---
  nav.quadPage = 0; nav.focus = (StatId)readoutAt(0, 0);
  struct { LogStatus s; const char* f; } logStates[] = {
    { LogStatus::On,     "statusbar_log_on.ppm" },     // green "LOG"  (logging OK)
    { LogStatus::Err,    "statusbar_log_err.ppm" },    // red "LOG"    (write error)
    { LogStatus::NoCard, "statusbar_no_sd.ppm" },      // grey "No SD Card"
    { LogStatus::Off,    "statusbar_log_off.ppm" },    // grey "LOG"   (not logging)
  };
  for (auto& ls : logStates) {
    g_snapLog = ls.s;
    ui::render(gs, nav, Theme::Day, hist, false);
    lv_refr_now(nullptr);
    dumpPpm(ls.f);
  }
  g_snapLog = LogStatus::On;   // restore for the remaining renders

  // Bottom-right cell focused: proves the highlight ring (and the cell itself)
  // fits on-screen below the header band — a top-cell-only cursor hid a
  // bottom-row overflow once.
  nav.quadPage = 0; nav.focus = (StatId)readoutAt(0, 3);
  ui::render(gs, nav, Theme::Day, hist, false);
  lv_refr_now(nullptr);
  dumpPpm("page0_day_focus3.ppm");
  // Focus view on TRANS — covers the trend graph (seeded history above) and the
  // top-right SD corner indicator.
  nav.view = View::Focus; nav.focus = (StatId)readoutAt(0, 0);
  ui::render(gs, nav, Theme::Day, hist, false);
  lv_refr_now(nullptr);
  dumpPpm("focus_day.ppm");

  // --- Alarm-zone trend graph (README figure: docs/images/alarm_zones.png) ---
  // TRANS thresholds are warnHi=240 / critHi=260 (gm_sierra_lz0.cpp). Feed its
  // 5-minute ring a green -> amber -> red ramp so focusChartDrawCb (ui.cpp),
  // which colours each line segment by the zone of its own sample, paints all
  // three alarm colours in one trace instead of the flat-green seeded history.
  {
    HistorySet alarmHist = hist;             // every other stat keeps its seeded trace
    GaugeSet   alarmGs   = gs;
    const float zGreen = 200.0f, zAmber = 245.0f, zRed = 270.0f;
    for (int s = 0; s < HISTORY_LEN; s++) {
      float t = (float)s / (float)(HISTORY_LEN - 1);   // 0 (5m ago) -> 1 (now)
      float v = (t < 0.5f) ? zGreen + (zAmber - zGreen) * (t / 0.5f)
                            : zAmber + (zRed   - zAmber) * ((t - 0.5f) / 0.5f);
      alarmHist.ring[(int)StatId::Trans].push(v);
    }
    // The focus chart only rebuilds its series when the sample clock advances
    // (ui.cpp guards on hist.lastSampleMs). focus_day.ppm above already rendered
    // TRANS at this same clock, so without bumping it the guard skips the rebuild
    // and the chart keeps the flat-green seeded trace — the injected ramp is
    // ignored. Advance the clock so the ramp is drawn and coloured per zone.
    alarmHist.lastSampleMs = hist.lastSampleMs + 1;
    alarmGs.g[(int)StatId::Trans].value = zRed;   // "now" sits in the red zone
    alarmGs.g[(int)StatId::Trans].peak  = zRed;
    nav.view = View::Focus; nav.focus = StatId::Trans;
    ui::render(alarmGs, nav, Theme::Day, alarmHist, false);
    lv_refr_now(nullptr);
    dumpPpm("alarm_zones.ppm");
  }

  // --- Individual tile alarm colours (README figure: warning_tile.png /
  // error_tile.png) — TRANS on the TOW quad page at 250°F (inside the 240-260
  // amber warn band) and 270°F (over the 260 crit red line). Everything else
  // on the page keeps its normal (green) reading for contrast.
  {
    GaugeSet warnGs = gs;
    warnGs.g[(int)StatId::Trans].value = 250; warnGs.g[(int)StatId::Trans].peak = 250;
    nav.view = View::Quad; nav.quadPage = 0; nav.focus = (StatId)readoutAt(0, 0);
    ui::render(warnGs, nav, Theme::Day, hist, false);
    lv_refr_now(nullptr);
    dumpPpm("warning_tile.ppm");

    GaugeSet errGs = gs;
    errGs.g[(int)StatId::Trans].value = 270; errGs.g[(int)StatId::Trans].peak = 270;
    ui::render(errGs, nav, Theme::Day, hist, false);
    lv_refr_now(nullptr);
    dumpPpm("error_tile.ppm");
  }

  nav.view = View::Quad;
  // Night theme sample on TOW
  nav.quadPage = 0; nav.focus = (StatId)readoutAt(0, 0);
  ui::render(gs, nav, Theme::Night, hist, false);
  lv_refr_now(nullptr);
  dumpPpm("page0_night.ppm");
  // Metric-mode samples (Units toggle): TOW/POWER/REGEN/RANGE cover every quantity tag.
  for (int p = 0; p <= 3; p++) {
    nav.quadPage = p; nav.focus = (StatId)readoutAt(p, 0);
    ui::render(gs, nav, Theme::Day, hist, true);
    lv_refr_now(nullptr);
    snprintf(path, sizeof path, "page%d_metric.ppm", p);
    dumpPpm(path);
  }
  // Settings menu (long-press overlay). Dash capabilities: RTC + SD log + OTA,
  // so every row is visible — this is the truck's real menu shape.
  menuSetCaps(/*hasRtc=*/true, /*hasSdLog=*/true, /*hasOta=*/true);
  MenuState menu{};
  Settings  settings{};
  ui::showMenu(menu, settings, Theme::Day, nav.rtcNow);
  lv_refr_now(nullptr);
  dumpPpm("menu_day.ppm");
  // Second frame with the cursor moved down, so the highlight and the rows
  // below the fold are both exercised (a top-row-only cursor hid the quad
  // overflow bug once).
  menuMove(menu, +4);
  ui::showMenu(menu, settings, Theme::Day, nav.rtcNow);
  lv_refr_now(nullptr);
  dumpPpm("menu_day_row4.ppm");
  ui::hideMenu();

  // Forget-adapter confirmation overlay.
  ui::showStatus("Adapter forgotten.\nScanning for a new one...", Theme::Day);
  lv_refr_now(nullptr);
  dumpPpm("forget_adapter.ppm");
  ui::showStatus(nullptr, Theme::Day);

  // Boot splash — rendered last so nothing else disturbs the screen it loads.
  ui::showSplash(nav.rtcNow, nav.rtcValid);
  lv_refr_now(nullptr);
  dumpPpm("splash_day.ppm");

  printf("wrote %d snapshots\n", readoutPageCount() + 13);
  return 0;
}

// Host millis() for LVGL's custom tick (monotonic fake clock).
extern "C" unsigned long millis(void) { static unsigned long t = 0; return t += 5; }

// Host stub: menu shows SD log status; no SD card on the host.
#include "sd_log.h"
LogStatus logStatus() { return g_snapLog; }
