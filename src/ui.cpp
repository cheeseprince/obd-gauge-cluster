#include <lvgl.h>
#include <cmath>
#include <cstdio>
#include "ui.h"
#include "board_caps.h"
#include "readouts.h"   // READOUTS[] is the source of truth for name/unit/decimals/thr/fullScale
#include "vehicle_profile.h" // VEHICLE (splash identity)
#include "vehicle_active.h" // VEHICLE/READOUTS/READOUT_COUNT macros over g_activeProfile
#include "vehicle_registry.h" // profileCount/profileLabelAt (Pick Vehicle overlay); profileKeyFor (tank scoping)
#include "pid_decode.h"     // effectiveTankGal (Fuel tank row + picker)
#include "alarm_holdoff.h"  // 4-second persistence gate for alarms
#include "alarm_ack.h"      // press-to-dismiss state for the alarm overlay
#include "sd_log.h"         // logStatus() for the menu Logging row

// File-scope hold-off instance (one shared instance so greens always reset timers).
static AlarmHoldoff alarmHold;

// Tracks whether any alarm is confirmed this frame; read by anyAlarm().
static bool g_anyAlarm = false;
// Press-to-dismiss + startup-grace state (see ui::ackAlarm / suppressAlarms).
static AlarmAck      g_alarmAck;
static volatile bool g_alarmShown     = false;   // overlay actually visible this frame (read on core 0 via alarmShown())
static bool          g_suppressAlarms = false;   // true during the 10s startup grace (core-1 only)

static lv_color_t zoneColor(Zone z, Theme t) {
  if (t == Theme::Night) return (z==Zone::Green) ? lv_color_hex(0xFF7A00)   // amber numerals
                                                  : lv_color_hex(0xFF2020); // red alert
  switch (z) { case Zone::Green: return lv_color_hex(0x33CC55);
               case Zone::Amber: return lv_color_hex(0xFFB000);
               default:          return lv_color_hex(0xFF3030); }
}

// Note: navigation is driven by the app's own tap/swipe detection (see main.cpp,
// using display::touch()), NOT by LVGL's gesture/click events — those proved
// unreliable on this resistive panel (missed swipes, wrong-cell clicks). The UI
// here is purely a passive renderer of GaugeSet + NavState.

// --- Status overlay: a full-screen label on lv_layer_top() shown while the OBD
// link is not yet up ("Connecting to OBD..." / "Scanning... see console").
// Mirrors the alarm overlay pattern exactly — built once in begin(), shown/hidden
// by showStatus(). When hidden, the gauges (and alarm overlay if active) show
// through normally. Both overlays sit on lv_layer_top(); when linkUp is false
// there are no alarm data, so ordering between them is a non-issue in practice.
static lv_obj_t* statusLabel = nullptr;

static void buildStatus() {
  // A full-screen label on lv_layer_top() — opaque black background, centered
  // white text in Montserrat 28. Hidden by default (added flag at build time).
  statusLabel = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_opa(statusLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(statusLabel, lv_color_black(), 0);
  lv_obj_set_size(statusLabel, 480, 320);
  lv_obj_set_style_text_color(statusLabel, lv_color_white(), 0);
  lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_20, 0);   // was _28 (fits 4 lines)
  lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(statusLabel, 95, 0);                          // was 130
  lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);      // hidden until OBD not linked
}

// --- Settings menu overlay: one opaque multi-line label on lv_layer_top(),
// rebuilt each frame from MenuState + Settings (text-mode menu). ---
static lv_obj_t* menuLabel = nullptr;

static void buildMenu() {
  menuLabel = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_opa(menuLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(menuLabel, lv_color_black(), 0);
  lv_obj_set_size(menuLabel, 480, 320);
  lv_obj_set_style_text_font(menuLabel, &lv_font_montserrat_18, 0);  // 18 (was 20): fit 12 rows on 320px
  lv_obj_set_style_text_align(menuLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_pad_left(menuLabel, 24, 0);
  // showMenu() renders a fixed-height WINDOW of rows (see there), so this pad
  // no longer has to shrink every time a menu row is added.
  lv_obj_set_style_pad_top(menuLabel, 14, 0);
  lv_obj_add_flag(menuLabel, LV_OBJ_FLAG_HIDDEN);
}

// --- Date/time editor overlay (opaque, on lv_layer_top(); mirrors the menu). ---
static lv_obj_t* timeSetLabel = nullptr;

static void buildTimeSet() {
  timeSetLabel = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_opa(timeSetLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(timeSetLabel, lv_color_black(), 0);
  lv_obj_set_size(timeSetLabel, 480, 320);
  lv_obj_set_style_text_font(timeSetLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(timeSetLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_pad_left(timeSetLabel, 24, 0);
  lv_obj_set_style_pad_top(timeSetLabel, 24, 0);
  lv_obj_add_flag(timeSetLabel, LV_OBJ_FLAG_HIDDEN);
}

// --- Vehicle-picker overlay (opaque, on lv_layer_top(); mirrors the time-set
// editor). Lists every registered profile; the caller marks the cursor row. ---
static lv_obj_t* vehPickLabel = nullptr;

static void buildVehPick() {
  vehPickLabel = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_opa(vehPickLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(vehPickLabel, lv_color_black(), 0);
  lv_obj_set_size(vehPickLabel, 480, 320);
  lv_obj_set_style_text_font(vehPickLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(vehPickLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_pad_left(vehPickLabel, 24, 0);
  lv_obj_set_style_pad_top(vehPickLabel, 24, 0);
  lv_obj_add_flag(vehPickLabel, LV_OBJ_FLAG_HIDDEN);
}

// --- Fuel-tank picker overlay. Same shape as the vehicle picker, but the list
// is longer than the screen (12 presets + Custom + Unset), so it renders a
// scrolling WINDOW that keeps the cursor in view rather than clipping. ---
static lv_obj_t* tankPickLabel = nullptr;

static void buildTankPick() {
  tankPickLabel = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_opa(tankPickLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(tankPickLabel, lv_color_black(), 0);
  lv_obj_set_size(tankPickLabel, 480, 320);
  lv_obj_set_style_text_font(tankPickLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(tankPickLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_pad_left(tankPickLabel, 24, 0);
  lv_obj_set_style_pad_top(tankPickLabel, 24, 0);
  lv_obj_add_flag(tankPickLabel, LV_OBJ_FLAG_HIDDEN);
}

// --- Boot splash: full-screen identity card on lv_layer_top(), shown for a few
// seconds at power-up while the OBD link comes up; then handed off to the
// connecting screen / gauges. Configured truck identity (trim/engine aren't
// OBD-detectable) + firmware version. ---
// Build stamp. FW_VERSION comes from fw_git.h (set by publish_ota.sh / the
// release workflow, since the flashed build has no .git) — the release tag
// for tagged builds, a "dev-<hash>" fallback otherwise; FW_DATE is injected
// by platformio.ini (crowpanel_obd), with a compile-date fallback for envs
// that don't set it.
#include "fw_git.h"
#ifndef FW_DATE
#define FW_DATE __DATE__
#endif
static lv_obj_t* splashScreen = nullptr;
static lv_obj_t* splashClock  = nullptr;   // updatable clock line, set by showSplash()

static void buildSplash(const Settings& cfg) {
  splashScreen = lv_obj_create(lv_layer_top());
  lv_obj_set_size(splashScreen, 480, 320);
  lv_obj_set_style_bg_color(splashScreen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(splashScreen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(splashScreen, 0, 0);
  lv_obj_set_style_radius(splashScreen, 0, 0);
  lv_obj_clear_flag(splashScreen, LV_OBJ_FLAG_SCROLLABLE);

  // A vehicle we identified but have no profile for names ITSELF here; anything
  // else falls back to the active profile's caption. This is the only place the
  // two can disagree -- a Ford Super Duty runs the Generic profile, so VEHICLE
  // would caption it "Generic" while the truck is perfectly well identified.
  // cfg's strings outlive this function (Settings is owned by main), so storing
  // the pointers in `lines` is safe; lv_label_set_text copies anyway.
  const bool detected = cfg.detectedName[0] != '\0';
  const char* idName   = detected ? cfg.detectedName   : VEHICLE.name;
  const char* idEngine = detected ? cfg.detectedEngine : VEHICLE.engine;

  struct SplashLine { const char* txt; const lv_font_t* font; uint32_t color; int y; };
  const SplashLine lines[] = {
    {idName,                 &lv_font_montserrat_28, 0xFFFFFF,  70},
    {idEngine,               &lv_font_montserrat_20, 0xFFB000, 118},
    {"OBD-II Monitor",       &lv_font_montserrat_20, 0xCCCCCC, 150},
    {"build " FW_DATE "  " FW_VERSION, &lv_font_montserrat_14, 0x888888, 210},
    {"connecting to OBD...", &lv_font_montserrat_14, 0x888888, 240},
  };
  for (const auto& l : lines) {
    lv_obj_t* lab = lv_label_create(splashScreen);
    lv_label_set_text(lab, l.txt);
    lv_obj_set_style_text_font(lab, l.font, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(l.color), 0);
    lv_obj_align(lab, LV_ALIGN_TOP_MID, 0, l.y);
  }
  splashClock = lv_label_create(splashScreen);          // clock line, updated by showSplash()
  lv_obj_set_style_text_font(splashClock, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(splashClock, lv_color_hex(0x888888), 0);
  lv_obj_align(splashClock, LV_ALIGN_TOP_MID, 0, 270);
  lv_obj_add_flag(splashScreen, LV_OBJ_FLAG_HIDDEN);   // shown by main() for the first few seconds
}

// --- Alarm overlay: a FULL-SCREEN takeover on lv_layer_top() (above quad AND
// focus). When any stat is in alarm it covers the whole screen and lists every
// active alarm, cascading top-down worst-first. No blinking. At most 4 stats
// have thresholds (trans/oil/coolant/volts) so ≤4 lines in practice; we make a
// slot per stat defensively. Built once; render() sets text/visibility only.
static lv_obj_t* alarmOverlay = nullptr;
static lv_obj_t* alarmLine[STAT_COUNT] = {nullptr};

static void buildAlarm() {
  alarmOverlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(alarmOverlay, 480, 320);              // full screen
  lv_obj_set_style_bg_opa(alarmOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(alarmOverlay, lv_color_black(), 0);
  lv_obj_set_style_border_width(alarmOverlay, 0, 0);
  lv_obj_set_style_radius(alarmOverlay, 0, 0);
  lv_obj_set_style_pad_all(alarmOverlay, 0, 0);
  lv_obj_clear_flag(alarmOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alarmOverlay, LV_OBJ_FLAG_HIDDEN);    // hidden until an alarm fires

  for (int i = 0; i < STAT_COUNT; i++) {
    alarmLine[i] = lv_label_create(alarmOverlay);
    lv_obj_set_style_text_font(alarmLine[i], &lv_font_montserrat_28, 0);
    lv_obj_set_pos(alarmLine[i], 16, 14 + i * 42);      // cascade top-down
    lv_obj_add_flag(alarmLine[i], LV_OBJ_FLAG_HIDDEN);
  }
}

// --- Focus screen widgets (built once in buildFocus, updated each frame in render) ---
static lv_obj_t*          focusScreen = nullptr;
static lv_obj_t*          fName   = nullptr;
static lv_obj_t*          fSd     = nullptr;   // SD status, top-right corner
static lv_obj_t*          fVal    = nullptr;
static lv_obj_t*          fChart     = nullptr;   // 5-min trend line graph
static lv_chart_series_t* fSeries    = nullptr;
static lv_obj_t*          fYLabel[4] = {nullptr, nullptr, nullptr, nullptr};  // manual Y-axis value labels
static lv_obj_t*          fPeak      = nullptr;   // bottom-left: peak

// Current Focus auto-fit scale + format, read by the chart axis-label callback.
// The chart's Y range is a fixed 0..1000 (we normalize); the callback maps each
// Y tick back to the real value using these, and labels X ticks as minutes-ago.
static float       g_focusLo  = 0.0f;
static float       g_focusHi  = 1.0f;
static int         g_focusDec = 0;
static Thresholds  g_focusThr;                 // thresholds of the focused stat
static Theme       g_focusTheme = Theme::Day;  // current theme (for zone colors)
static Quantity    g_focusQty    = Quantity::None;
static bool        g_focusMetric = false;

// Chart draw callback. Two jobs:
//  - TICK_LABEL: relabel Y ticks with real values, X ticks with time ("5m"…"now").
//  - LINE_AND_POINT: colour each line segment by the zone of its sample value,
//    so the trace shows green/amber/red where the value was in each zone.
static void focusChartDrawCb(lv_event_t* e) {
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);

  // Per-segment colour by zone.
  if (lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_LINE_AND_POINT)) {
    if (dsc->line_dsc && fSeries) {
      lv_coord_t* ya = lv_chart_get_y_array(fChart, fSeries);
      lv_coord_t  yv = ya[dsc->id];
      if (yv != LV_CHART_POINT_NONE) {
        float real = g_focusLo + (yv / 1000.0f) * (g_focusHi - g_focusLo);
        dsc->line_dsc->color = zoneColor(zoneFor(real, g_focusThr), g_focusTheme);
      }
    }
    return;
  }

  // Axis tick labels. NOTE: use standard snprintf, NOT lv_snprintf — LVGL's
  // lv_snprintf is built with LV_SPRINTF_USE_FLOAT=0, so "%f" degrades to "f".
  if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_TICK_LABEL)) return;
  if (dsc->text == nullptr) return;
  if (dsc->id == LV_CHART_AXIS_PRIMARY_Y) {
    float real = g_focusLo + (dsc->value / 1000.0f) * (g_focusHi - g_focusLo);
    snprintf(dsc->text, dsc->text_length, g_focusDec ? "%.1f" : "%.0f",
             (double)toDisplayValue(g_focusQty, real, g_focusMetric));
  } else if (dsc->id == LV_CHART_AXIS_PRIMARY_X) {
    int minsAgo = 5 - (int)dsc->value;            // 6 ticks: left=5m ago … right=now
    if (minsAgo <= 0) snprintf(dsc->text, dsc->text_length, "now");
    else              snprintf(dsc->text, dsc->text_length, "%dm", minsAgo);
  }
}

// One quad cell = container + name + value + bar + peak. Built once, updated each frame.
struct Cell { lv_obj_t* box; lv_obj_t* name; lv_obj_t* val; lv_obj_t* bar; lv_obj_t* peak; };
static Cell cells[4];
static lv_obj_t* quadScreen = nullptr;
static lv_obj_t* pageLabel  = nullptr;   // header-band page name (quad view)
static lv_obj_t* btLabel    = nullptr;   // header-band Bluetooth link icon, far left
static lv_obj_t* clockLabel = nullptr;   // header-band clock, left (RTC boards)
static lv_obj_t* sdLabel    = nullptr;   // header-band SD/logging status, right

static void buildQuad() {
  quadScreen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(quadScreen, lv_color_black(), 0);
  // Disable scrolling on the quad screen — prevents stray scrollbars and
  // ensures swipe gestures (nav) are not stolen by the default scrollable screen.
  lv_obj_clear_flag(quadScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(quadScreen, LV_SCROLLBAR_MODE_OFF);
  static lv_coord_t col[] = {236, 236, LV_GRID_TEMPLATE_LAST};
  // Row 0 is a 24px header band (page name + number, both columns); the gauge
  // cells live in rows 1-2. Giving the header its own grid row keeps it from
  // ever overlapping a cell, so the encoder highlight ring stays an unbroken
  // rectangle (every cell corner is occupied: names top-left, peaks
  // bottom-right, bars bottom-center — a floating chip would collide).
  // Geometry is EXPLICIT so it sums to the panel exactly — the theme's implicit
  // grid gaps pushed the bottom row (and its highlight ring) past y=320:
  // height 24 + 4 + 144 + 4 + 144 = 320, width 2 + 236 + 4 + 236 + 2 = 480.
  static lv_coord_t row[] = {24, 144, 144, LV_GRID_TEMPLATE_LAST};
  lv_obj_set_style_pad_all(quadScreen, 0, 0);
  lv_obj_set_style_pad_left(quadScreen, 2, 0);
  lv_obj_set_style_pad_right(quadScreen, 2, 0);
  lv_obj_set_style_pad_row(quadScreen, 4, 0);
  lv_obj_set_style_pad_column(quadScreen, 4, 0);
  lv_obj_set_grid_dsc_array(quadScreen, col, row);
  lv_obj_set_layout(quadScreen, LV_LAYOUT_GRID);
  for (int i = 0; i < 4; i++) {
    Cell& c = cells[i];
    c.box = lv_obj_create(quadScreen);
    lv_obj_set_grid_cell(c.box, LV_GRID_ALIGN_STRETCH, i%2, 1, LV_GRID_ALIGN_STRETCH, 1 + i/2, 1);
    lv_obj_set_style_bg_color(c.box, lv_color_hex(0x101010), 0);
    lv_obj_set_style_border_width(c.box, 0, 0);
    // Keep cells non-scrollable (consistent with the screen itself).
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_SCROLLABLE);
    c.name = lv_label_create(c.box); lv_obj_align(c.name, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(c.name, &lv_font_montserrat_20, 0);
    c.val  = lv_label_create(c.box); lv_obj_align(c.val, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_text_font(c.val, &lv_font_montserrat_48, 0);
    c.bar  = lv_bar_create(c.box);  lv_obj_set_size(c.bar, 180, 8);
    lv_bar_set_range(c.bar, 0, 100);   // fixed range — set once, not per frame
    lv_obj_align(c.bar, LV_ALIGN_BOTTOM_MID, 0, -18);
    c.peak = lv_label_create(c.box); lv_obj_align(c.peak, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_text_font(c.peak, &lv_font_montserrat_14, 0);
  }
  // Page-name header: grid row 0 spanning both columns — a full-width bar with
  // a background so it reads as a title, not a gauge. Text + theme colors are
  // set in render(). montserrat_20 is the closest to "bold" LVGL ships
  // (all bundled Montserrat weights are regular; size + bright-on-background
  // carries the emphasis).
  pageLabel = lv_label_create(quadScreen);
  lv_obj_set_grid_cell(pageLabel, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 0, 1);
  lv_obj_set_style_text_font(pageLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(pageLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_opa(pageLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_top(pageLabel, 2, 0);
  lv_label_set_text(pageLabel, "");
  // Status-bar side items, children of the band so they inherit its background
  // and vertical centering: clock left (RTC boards only — rtcValid never sets on
  // the others), SD/logging state right. Font 14 + dim colors keep the centered
  // page name dominant; texts and colors are set per frame in render().
  // Bluetooth link icon (far left): blue when the OBD adapter is linked, gray
  // when not. Color is refreshed each frame by showStatus(), which knows the
  // link state (msg == nullptr means linked).
  btLabel = lv_label_create(pageLabel);
  lv_obj_set_style_text_font(btLabel, &lv_font_montserrat_20, 0);
  lv_obj_align(btLabel, LV_ALIGN_LEFT_MID, 6, 0);
  lv_label_set_text(btLabel, LV_SYMBOL_BLUETOOTH);
  lv_obj_set_style_text_color(btLabel, lv_color_hex(0x666666), 0);   // gray until linked
  // Clock (left, after the BT icon): same size as the page name (montserrat_20).
  clockLabel = lv_label_create(pageLabel);
  lv_obj_set_style_text_font(clockLabel, &lv_font_montserrat_20, 0);
  lv_obj_align(clockLabel, LV_ALIGN_LEFT_MID, 34, 0);
  lv_label_set_text(clockLabel, "");
  sdLabel = lv_label_create(pageLabel);
  lv_obj_set_style_text_font(sdLabel, &lv_font_montserrat_14, 0);
  lv_obj_align(sdLabel, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_label_set_text(sdLabel, "");
}

// Build the Focus screen once: stat name (top), big current value, a 5-minute
// trend line graph, and a bottom row (peak left, auto-fit range + window right).
// Widgets are placed once; values/series are filled in by render() each frame.
static void buildFocus() {
  focusScreen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(focusScreen, lv_color_black(), 0);
  // Disable scrolling — same pattern as quadScreen.
  lv_obj_clear_flag(focusScreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(focusScreen, LV_SCROLLBAR_MODE_OFF);

  fName = lv_label_create(focusScreen);
  lv_obj_align(fName, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_text_font(fName, &lv_font_montserrat_28, 0);

  fVal = lv_label_create(focusScreen);
  lv_obj_align(fVal, LV_ALIGN_TOP_MID, 0, 42);
  lv_obj_set_style_text_font(fVal, &lv_font_montserrat_48, 0);

  // 5-minute trend graph. We normalise samples to 0..1000 ourselves (auto-fit),
  // so the chart's Y range is a fixed 0..1000 and the line always fills the height.
  // The draw callback relabels the Y ticks with real values and X ticks with time.
  fChart = lv_chart_create(focusScreen);
  lv_obj_set_size(fChart, 440, 156);
  lv_obj_align(fChart, LV_ALIGN_TOP_MID, 0, 96);
  lv_chart_set_type(fChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(fChart, HISTORY_LEN);
  lv_chart_set_range(fChart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
  lv_chart_set_div_line_count(fChart, 0, 6);            // vertical time grid only;
                                                        // no horizontal lines (they cut through the Y labels)
  lv_obj_set_style_bg_color(fChart, lv_color_hex(0x0A0A0A), 0);
  lv_obj_set_style_border_width(fChart, 0, 0);
  lv_obj_set_style_size(fChart, 0, LV_PART_INDICATOR);  // hide per-point dots
  lv_obj_set_style_line_width(fChart, 2, LV_PART_ITEMS);
  // Axis ticks with labels: Y = 5 majors (value labels), X = 6 majors (time).
  // Padding leaves room for the tick labels (left for Y, bottom for X).
  // X axis: built-in time tick labels (these render fine). Y axis: ticks only,
  // NO built-in labels — LVGL clips them to the chart box on this build, so we
  // draw the Y value labels ourselves as plain labels (fYLabel) to the left.
  // Y: NO tick marks (len 0) — they'd draw a line through our manual Y labels.
  lv_chart_set_axis_tick(fChart, LV_CHART_AXIS_PRIMARY_Y, 0, 0, 4, 0, false, 4);
  lv_chart_set_axis_tick(fChart, LV_CHART_AXIS_PRIMARY_X, 4, 2, 6, 1, true, 36);
  lv_obj_set_style_pad_left(fChart, 52, 0);   // inset plot so our Y labels have room
  lv_obj_set_style_pad_bottom(fChart, 26, 0); // room for the larger X time labels
  lv_obj_set_style_pad_right(fChart, 16, 0);
  lv_obj_set_style_pad_top(fChart, 12, 0);    // room so the top trace isn't clipped
  // Larger X time-tick labels for readability (built-in Montserrat is regular weight).
  lv_obj_set_style_text_font(fChart, &lv_font_montserrat_20, LV_PART_TICKS);
  lv_obj_set_style_text_color(fChart, lv_color_hex(0xBBBBBB), LV_PART_TICKS);
  lv_obj_add_event_cb(fChart, focusChartDrawCb, LV_EVENT_DRAW_PART_BEGIN, nullptr);
  fSeries = lv_chart_add_series(fChart, lv_color_hex(0x33CC55), LV_CHART_AXIS_PRIMARY_Y);

  // Manual Y-axis value labels, right-aligned just left of the plot. The plot
  // spans screen y 108..226 (chart y96 + pad_top12 .. + h156 - pad_bottom26);
  // 4 labels at the gridline levels: hi, +2/3, +1/3, lo. Text set in render().
  // Positioned far left of the centered big value, so no horizontal overlap.
  static const int yPos[4] = {95, 134, 174, 213};    // label top = gridline_y - 13 (font 20)
  for (int i = 0; i < 4; i++) {
    fYLabel[i] = lv_label_create(focusScreen);
    lv_obj_set_width(fYLabel[i], 52);
    lv_obj_set_style_text_align(fYLabel[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(fYLabel[i], &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(fYLabel[i], lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_pos(fYLabel[i], 8, yPos[i]);           // right edge ~x60, plot starts x72
  }

  fPeak = lv_label_create(focusScreen);
  lv_obj_align(fPeak, LV_ALIGN_BOTTOM_LEFT, 8, -6);
  lv_obj_set_style_text_font(fPeak, &lv_font_montserrat_20, 0);

  // SD status in the free top-right corner — you can sit in Focus for a whole
  // tow, so a card failure must stay visible here too. Text set in render().
  fSd = lv_label_create(focusScreen);
  lv_obj_set_style_text_font(fSd, &lv_font_montserrat_14, 0);
  lv_obj_align(fSd, LV_ALIGN_TOP_RIGHT, -8, 8);
  lv_label_set_text(fSd, "");
}

// SD/logging status for a status label, exception-highlighted: dim "LOG" while
// recording, bright "SD!" when the card needs attention (missing/write error),
// nothing when off/idle. Shared by the quad header bar and the Focus corner.
// Compile-gated: boards without the SD logger never run logTick(), so s_status
// would sit at its NoCard initial forever and false-alarm.
static void updateSdLabel(lv_obj_t* lbl, Theme theme) {
#if HAS_SD_LOG
  (void)theme;   // status colors are semantic (green/red/grey), theme-independent
  switch (logStatus()) {
    case LogStatus::On:                             // logging enabled AND writing OK
      lv_label_set_text(lbl, "LOG");
      lv_obj_set_style_text_color(lbl, lv_color_hex(0x00C853), 0);   // green
      break;
    case LogStatus::Err:                            // card present but a write failed
      lv_label_set_text(lbl, "LOG");
      lv_obj_set_style_text_color(lbl, lv_color_hex(0xFF3B30), 0);   // red
      break;
    case LogStatus::NoCard:                         // no SD card detected
      lv_label_set_text(lbl, "No SD Card");
      lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);   // grey
      break;
    default:                                        // Off — logging not enabled
      lv_label_set_text(lbl, "LOG");
      lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);   // grey
      break;
  }
#else
  (void)lbl; (void)theme;
#endif
}

// Name of the bonded adapter for the "Forget adapter?" confirm row. There is
// NO fallback chain here — adapter identity is not reachable from ui.cpp at
// all, so this always returns the plain literal "adapter". Investigated
// 2026-08-11: the obvious source is g_obd.connStatus().addr (and even that is
// only a MAC — there is no cached adapter *name* anywhere in the firmware),
// but g_obd is `static` inside main.cpp, i.e. it has internal linkage and is
// not visible from this translation unit at all. MenuState/Settings (what
// showMenu() is actually given) carry no adapter identity either. Reaching
// it would mean adding a new cross-file accessor purely to grow this string,
// which is a separate change from the confirm dialog itself -- so naming the
// adapter is deferred until that plumbing is deliberately added.
static const char* obdAdapterLabel() { return "adapter"; }

namespace ui {

// anyAlarm: returns true if any stat has a hold-off-confirmed alarm this frame.
// The result is set by the alarm-overlay block in render() via g_anyAlarm.
// gs param is kept for interface compatibility but is unused here.
bool anyAlarm(const GaugeSet& gs) {
  (void)gs;
  return g_anyAlarm;
}

// Startup grace: while true, the alarm overlay is forced hidden (set by main()
// for 10s after link-up so the key-on voltage dip can't trip a warning).
void suppressAlarms(bool suppress) { g_suppressAlarms = suppress; }

// Acknowledge (dismiss) the currently-shown alarm. It stays hidden until all
// alarms clear, then a fresh alarm re-shows.
void ackAlarm() { g_alarmAck.ack(); }

// True if the alarm overlay is on screen right now (so a press dismisses it
// instead of navigating).
bool alarmShown() { return g_alarmShown; }

// Build all static screen objects on first boot; load quad as the starting screen.
void begin(const Settings& s) {
  buildQuad();
  buildFocus();
  buildAlarm();            // alarm banner on lv_layer_top() — created once, hidden by default
  buildStatus();           // status overlay on lv_layer_top() — hidden by default
  buildMenu();             // settings menu overlay on lv_layer_top() — hidden by default
  buildTimeSet();          // date/time editor overlay on lv_layer_top() — hidden by default
  buildVehPick();          // vehicle-picker overlay on lv_layer_top() — hidden by default
  buildTankPick();         // fuel-tank capacity picker overlay — hidden by default
  buildSplash(s);          // boot splash on lv_layer_top() — shown by main() at power-up
  lv_scr_load(quadScreen);
}

// Show or hide the full-screen status overlay. Called each frame by loop():
//   nullptr → hide (OBD link is up — gauges and alarms show normally).
//   non-null → set text and show (OBD not linked — blocks gauges from view).
// Theme-aware: night renders amber like every other screen (the OTA/connect
// status previously stayed white and blinded night vision).
void showStatus(const char* msg, Theme t) {
  // BT link icon: blue when linked (msg == nullptr), gray when not/connecting.
  if (btLabel)
    lv_obj_set_style_text_color(btLabel,
        msg ? lv_color_hex(0x666666) : lv_color_hex(0x2196F3), 0);
  if (!msg) {
    lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_set_style_text_color(statusLabel,
    t == Theme::Night ? lv_color_hex(0xFF7A00) : lv_color_white(), 0);
  lv_label_set_text(statusLabel, msg);
  lv_obj_clear_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
}

void showMenu(const MenuState& m, const Settings& s, Theme t, const DateTime& now) {
  lv_color_t fg = (t == Theme::Day) ? lv_color_white() : lv_color_hex(0xFF7A00);
  lv_obj_set_style_text_color(menuLabel, fg, 0);

  auto cur = [&](MenuItem it) { return m.sel == (uint8_t)it ? ">" : " "; };
  char nightVal[12];  snprintf(nightVal,  sizeof nightVal,  "[ %s ]", nightModeLabel(s));
  char brightVal[12]; snprintf(brightVal, sizeof brightVal, "[ %u%% ]", (unsigned)s.brightnessPct);
  char unitsVal[16];  snprintf(unitsVal,  sizeof unitsVal,  "[ %s ]", s.metric ? "metric" : "imperial");
  // Destructive rows open an inline Yes/No dialog. Naming the adapter matters:
  // confirming "Forget adapter?" tells you nothing about WHICH adapter you are
  // about to drop.
  char resetTxtBuf[40], forgetTxtBuf[48];
  const char* resetTxt = "Reset trip";
  if (m.armed == MenuItem::ResetTrip) {
    snprintf(resetTxtBuf, sizeof resetTxtBuf, "Reset trip?   < %s >",
             m.confirmYes ? "Yes" : "No");
    resetTxt = resetTxtBuf;
  }
  const char* forgetTxt = "Forget adapter";
  if (m.armed == MenuItem::ForgetAdapter) {
    // obdAdapterLabel() only ever returns the 7-char literal "adapter" today
    // (see its doc comment — there is no name/MAC plumbing to this file yet),
    // so %.16s is not doing anything for the current caller. It stays as a
    // defensive bound on forgetTxtBuf (48 B) for whenever that plumbing is
    // added and `who` becomes a real MAC ("aa:bb:cc:dd:ee:ff", 17 chars) or an
    // adapter name of unknown length — cheap insurance now, not dead code.
    const char* who = obdAdapterLabel();
    snprintf(forgetTxtBuf, sizeof forgetTxtBuf, "Forget %.16s?  < %s >",
             who, m.confirmYes ? "Yes" : "No");
    forgetTxt = forgetTxtBuf;
  }
  // Fuel tank shows the EFFECTIVE capacity — user override if set for this
  // vehicle, else the profile's factory figure. The row doubles as a readout of
  // what DIESEL FILL is actually using, which is how a mis-set value becomes
  // visible instead of silently scaling every gallons figure.
  char tankVal[16];
  {
    float gal = effectiveTankGal(tankOverrideFor(s, profileKeyFor(&VEHICLE)),
                                 VEHICLE.dieselTankGal);
    if (gal > 0.0f) snprintf(tankVal, sizeof tankVal, "[ %.1f gal ]", gal);
    else            snprintf(tankVal, sizeof tankVal, "[ SET UP ]");
  }
  char clk[24];
  if (now.y >= 2000) formatDateTime(now, clk, sizeof clk); else snprintf(clk, sizeof clk, "--:--");
  // Show the user's intent (ON/OFF) — the runtime status only overrides for a
  // missing card or a write error. (logStatus() is Off whenever not actively
  // writing, e.g. disconnected, so it can't drive the ON/OFF label.)
  const char* logTxt;
  switch (logStatus()) {
    case LogStatus::NoCard: logTxt = "[ NO SD ]"; break;
    case LogStatus::Err:    logTxt = "[ ERR ]";   break;
    default:                logTxt = s.logging ? "[ ON ]" : "[ OFF ]"; break;
  }

  // One table instead of a fixed 13-row format string. The menu had outgrown
  // the panel: every added row ate another 22 px of a 320 px screen, and the
  // Fuel tank row left no headroom at all -- the next row would have pushed
  // Close off the bottom, invisibly. Rendering a WINDOW of rows around the
  // cursor decouples the row count from the screen height for good.
  //
  // The knob already scrolled past the bottom (menuMove wraps and skips hidden
  // rows); only the drawing was stuck showing everything at once.
  struct MRow { MenuItem it; const char* name; const char* val; };
  const MRow rows[] = {
    { MenuItem::NightMode,     "Night mode",    nightVal  },
    { MenuItem::Brightness,    "Brightness",    brightVal },
    { MenuItem::Units,         "Units",         unitsVal  },
    { MenuItem::FuelTank,      "Fuel tank",     tankVal   },
    { MenuItem::SetTime,       "Set date/time", nullptr   },
    { MenuItem::Logging,       "Logging",       logTxt    },
    { MenuItem::ResetTrip,     resetTxt,        nullptr   },   // whole-row text (may be a dialog)
    { MenuItem::ForgetAdapter, forgetTxt,       nullptr   },   // ditto
    { MenuItem::PickVehicle,   "Pick vehicle",  nullptr   },
    { MenuItem::WifiSetup,     "WiFi setup",    nullptr   },
    { MenuItem::CheckUpdate,   "Check update / time sync", nullptr },
    { MenuItem::Version,       "Version",       nullptr   },
    { MenuItem::Close,         "Close",         nullptr   },
  };
  constexpr int MENU_ROWS = (int)(sizeof(rows) / sizeof(rows[0]));
  // Adding a MenuItem without a row here would silently drop it from the menu.
  static_assert(MENU_ROWS == (int)MenuItem::COUNT,
                "every MenuItem needs exactly one row in showMenu()");

  // Collapse to the rows this board actually shows (SetTime needs an RTC,
  // Logging an SD slot, WiFi/Update the OTA stack), and locate the cursor
  // among them -- the window must be computed over VISIBLE rows, or a hidden
  // row would leave a blank line and shift everything below it.
  int visIdx[MENU_ROWS], nVis = 0, cursor = 0;
  for (int i = 0; i < MENU_ROWS; i++) {
    if (!menuItemVisible(rows[i].it)) continue;
    if (m.sel == (uint8_t)rows[i].it) cursor = nVis;
    visIdx[nVis++] = i;
  }

  // 10 rows + header + up to two "more" markers = 13 lines at montserrat_18
  // (~22 px) = ~286 px, inside the 320 px panel with the top pad.
  const int VIS = 10;
  int top = menuWindowTop(cursor, nVis, VIS);
  int bot = top + VIS;
  if (bot > nVis)       bot = nVis;

  char buf[900];
  int p = snprintf(buf, sizeof buf, "SETTINGS            %s\n", clk);
  // Count the hidden rows rather than just pointing: "3 more" tells the user
  // how far the list goes, which a bare arrow does not.
  if (top > 0)
    p += snprintf(buf + p, sizeof buf - p, "  ^ %d more\n", top);
  for (int k = top; k < bot && p < (int)sizeof buf - 80; k++) {
    const MRow& r = rows[visIdx[k]];
    if (r.val) p += snprintf(buf + p, sizeof buf - p, "%s %-16s%s\n", cur(r.it), r.name, r.val);
    else       p += snprintf(buf + p, sizeof buf - p, "%s %s\n",      cur(r.it), r.name);
  }
  if (bot < nVis)
    snprintf(buf + p, sizeof buf - p, "  v %d more", nVis - bot);

  lv_label_set_text(menuLabel, buf);
  lv_obj_clear_flag(menuLabel, LV_OBJ_FLAG_HIDDEN);
}

void hideMenu() { lv_obj_add_flag(menuLabel, LV_OBJ_FLAG_HIDDEN); }

void showTimeSet(const DateTime& t, uint8_t f, Theme th) {
  char fld[5][8];
  snprintf(fld[0], 8, "%04d", t.y);
  snprintf(fld[1], 8, "%02u", (unsigned)t.mon);
  snprintf(fld[2], 8, "%02u", (unsigned)t.d);
  snprintf(fld[3], 8, "%02u", (unsigned)t.h);
  snprintf(fld[4], 8, "%02u", (unsigned)t.min);
  char w[5][12];
  for (int i = 0; i < 5; i++) snprintf(w[i], 12, (i == f) ? "[%s]" : " %s ", fld[i]);

  char buf[160];
  snprintf(buf, sizeof buf,
    "SET DATE / TIME\n\n"
    "%s-%s-%s   %s:%s\n\n"
    "rotate=change  click=next  hold=save",
    w[0], w[1], w[2], w[3], w[4]);
  lv_label_set_text(timeSetLabel, buf);
  lv_obj_set_style_text_color(timeSetLabel,
      (th == Theme::Day) ? lv_color_white() : lv_color_hex(0xFF7A00), 0);
  lv_obj_clear_flag(timeSetLabel, LV_OBJ_FLAG_HIDDEN);
}

void hideTimeSet() { lv_obj_add_flag(timeSetLabel, LV_OBJ_FLAG_HIDDEN); }

void showVehiclePick(uint8_t sel, bool autoOn, Theme th) {
  char buf[512];
  int p = snprintf(buf, sizeof buf, "SELECT VEHICLE\n\n");
  p += snprintf(buf + p, sizeof buf - p, "%s Auto-detect%s\n",
                sel == 0 ? ">" : " ", autoOn ? " *" : "");
  for (int i = 0; i < profileCount() && p < (int)sizeof buf - 40; i++)
    p += snprintf(buf + p, sizeof buf - p, "%s %s\n",
                  (uint8_t)(i + 1) == sel ? ">" : " ", profileLabelAt(i));
  snprintf(buf + p, sizeof buf - p, "\nrotate=move  click=select+reboot  hold=cancel");
  lv_label_set_text(vehPickLabel, buf);
  lv_obj_set_style_text_color(vehPickLabel,
      (th == Theme::Day) ? lv_color_white() : lv_color_hex(0xFF7A00), 0);
  lv_obj_clear_flag(vehPickLabel, LV_OBJ_FLAG_HIDDEN);
}

void hideVehiclePick() { lv_obj_add_flag(vehPickLabel, LV_OBJ_FLAG_HIDDEN); }

void showTankPick(const TankPickState& t, Theme th) {
  // The list does not fit the screen, so show a window of rows that keeps the
  // cursor inside it. Centring the cursor (rather than paging) means a turn
  // always visibly moves something, which is what tells the user the knob is
  // doing anything at the ends of a long list.
  const int VIS = 8;
  const int n   = tankRowCount();
  const int top = menuWindowTop((int)t.sel, n, VIS);

  char buf[768];
  int p = snprintf(buf, sizeof buf, "FUEL TANK CAPACITY\n\n");
  for (int i = top; i < top + VIS && i < n && p < (int)sizeof buf - 64; i++) {
    const char* mark = ((uint8_t)i == t.sel) ? ">" : " ";
    if (i < TANK_PRESET_COUNT)
      p += snprintf(buf + p, sizeof buf - p, "%s %5.1f gal   %s\n",
                    mark, TANK_PRESETS[i], TANK_HINTS[i]);
    else if (i == tankRowCustom())
      // While editing, the value is live and bracketed so it is obvious the
      // knob is changing THIS and not the row cursor.
      p += snprintf(buf + p, sizeof buf - p, t.editing ? "%s Custom  < %.1f gal >\n"
                                                       : "%s Custom  ( %.1f gal )\n",
                    mark, t.customGal);
    else
      p += snprintf(buf + p, sizeof buf - p, "%s Not set   (hide the tile)\n", mark);
  }
  snprintf(buf + p, sizeof buf - p,
           t.editing ? "\nrotate=adjust  click=save  hold=cancel"
                     : "\nrotate=move  click=select  hold=cancel");
  lv_label_set_text(tankPickLabel, buf);
  lv_obj_set_style_text_color(tankPickLabel,
      (th == Theme::Day) ? lv_color_white() : lv_color_hex(0xFF7A00), 0);
  lv_obj_clear_flag(tankPickLabel, LV_OBJ_FLAG_HIDDEN);
}

void hideTankPick() { lv_obj_add_flag(tankPickLabel, LV_OBJ_FLAG_HIDDEN); }

void showSplash(const DateTime& now, bool rtcValid) {
  // Gate on rtcValid, not now.y: NavState seeds rtcNow with a plausible
  // placeholder date, so a year check shows a FAKE date before the first
  // real RTC read (and forever on a no-RTC board).
  char clk[24];
  if (rtcValid) formatDateTime(now, clk, sizeof clk); else snprintf(clk, sizeof clk, "--:--");
  lv_label_set_text(splashClock, clk);
  lv_obj_clear_flag(splashScreen, LV_OBJ_FLAG_HIDDEN);
}
void hideSplash() { lv_obj_add_flag(splashScreen, LV_OBJ_FLAG_HIDDEN); }

void render(const GaugeSet& gs, const NavState& navState, Theme theme, const HistorySet& hist, bool metric) {
  if (navState.view != View::Focus) {
    // --- Quad page ---
    if (lv_scr_act() != quadScreen) lv_scr_load(quadScreen);
    {
      char pbuf[24];
      snprintf(pbuf, sizeof pbuf, "%s  %d/%d", pageName(navState.quadPage),
               navState.quadPage + 1, readoutPageCount());
      lv_label_set_text(pageLabel, pbuf);
      // Bright text on a dark bar in both themes (night stays amber-family).
      lv_obj_set_style_text_color(pageLabel,
          (theme==Theme::Day) ? lv_color_white() : lv_color_hex(0xFF7A00), 0);
      lv_obj_set_style_bg_color(pageLabel,
          (theme==Theme::Day) ? lv_color_hex(0x262626) : lv_color_hex(0x1C0E00), 0);
      // Side items: dimmer than the page name so they read as chrome, not data.
      lv_color_t dim = (theme==Theme::Day) ? lv_color_hex(0xAAAAAA) : lv_color_hex(0xB35400);
      // Clock (left): only once the RTC has produced a real time — "--:--"
      // would be permanent noise on boards without one.
      if (navState.rtcValid) {
        char cbuf[8];
        snprintf(cbuf, sizeof cbuf, "%02u:%02u",
                 (unsigned)navState.rtcNow.h, (unsigned)navState.rtcNow.min);
        lv_label_set_text(clockLabel, cbuf);
        lv_obj_set_style_text_color(clockLabel, dim, 0);
      } else {
        lv_label_set_text(clockLabel, "");
      }
      updateSdLabel(sdLabel, theme);
    }
    for (int i = 0; i < 4; i++) {
      int idx = readoutAt(navState.quadPage, i);
      if (idx < 0) {                       // empty cell on a partial page -> blank it
        lv_label_set_text(cells[i].name, "");
        lv_label_set_text(cells[i].val, "");
        lv_label_set_text(cells[i].peak, "");
        lv_obj_add_flag(cells[i].bar, LV_OBJ_FLAG_HIDDEN);   // hide the track too (value 0 still drew the empty pill)
#if HAS_KNOB_MENU
        lv_obj_set_style_border_width(cells[i].box, 0, 0);
#endif
        continue;
      }
      StatId id = (StatId)idx;
      lv_obj_clear_flag(cells[i].bar, LV_OBJ_FLAG_HIDDEN);   // un-hide after an empty-cell page
#if HAS_KNOB_MENU
      // Highlight cursor: a border marks the encoder-focused cell (Quad view).
      lv_obj_set_style_border_color(cells[i].box, lv_color_hex(0x00E0FF), 0);
      lv_obj_set_style_border_width(cells[i].box, (id == navState.focus) ? 3 : 0, 0);
#endif
      const ReadoutDef& r = READOUTS[(int)id];  // table is source of truth
      const Gauge& g = gs.g[(int)id];
      lv_color_t base = (theme==Theme::Day) ? lv_color_white() : lv_color_hex(0xFF7A00);
      lv_label_set_text(cells[i].name, statLabel((int)id));   // display text, not the log key
      lv_obj_set_style_text_color(cells[i].name, base, 0);
      char buf[24];
      if (!g.valid) {
        // No reading yet (unsupported/unread PID) — show "--", grey, empty bar.
        // EXCEPT when the row is blocked on a SETTING rather than on the truck
        // (a FILL row with no tank capacity): say so, in the accent colour, so
        // it reads as actionable rather than dead. This is a prompt, not a
        // reading — it is deliberately not a number, so it cannot be mistaken
        // for data the way a defaulted capacity's "0.0 gal" would be.
        snprintf(buf, sizeof buf, g.needsSetup ? "SET UP" : "--");
        lv_label_set_text(cells[i].val, buf);
        lv_obj_set_style_text_color(cells[i].val,
            g.needsSetup ? lv_color_hex(0xFF7A00) : lv_color_hex(0x666666), 0);
        lv_bar_set_value(cells[i].bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(cells[i].bar, lv_color_hex(0x333333), LV_PART_INDICATOR);
        lv_label_set_text(cells[i].peak, "");
        continue;
      }
      Zone z = zoneForStat(gs, (int)id);  // zone from canonical value (unit-independent; OIL P gated engine-off)
      float dv = toDisplayValue(r.quantity, g.value, metric);
      const char* u = displayUnit(r, metric);
      snprintf(buf, sizeof buf, (r.decimals? "%.1f%s":"%.0f%s"), dv, u);
      lv_label_set_text(cells[i].val, buf);
      // Stale (held value, dead PID while linked): number stays but goes grey —
      // visibly not-live without the "--" blank/flicker.
      lv_obj_set_style_text_color(cells[i].val,
        g.stale ? lv_color_hex(0x666666) : zoneColor(z, theme), 0);
      float full = r.fullScale;            // bar stays canonical (fill fraction)
      int pct = (int)(100.0f * g.value / full); pct = pct<0?0:pct>100?100:pct;
      lv_bar_set_value(cells[i].bar, pct, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(cells[i].bar,
        g.stale ? lv_color_hex(0x333333) : zoneColor(z, theme), LV_PART_INDICATOR);
      char pk[24]; snprintf(pk, sizeof pk, (r.decimals ? "peak %.1f%s" : "peak %.0f%s"),
                            toDisplayValue(r.quantity, g.peak, metric), displayUnit(r, metric));
      lv_label_set_text(cells[i].peak, pk);
      lv_obj_set_style_text_color(cells[i].peak, lv_color_hex(0x888888), 0);
    }
  } else {
    // --- Focus view ---
    if (lv_scr_act() != focusScreen) lv_scr_load(focusScreen);
    StatId id = navState.focus;
    const ReadoutDef& r = READOUTS[(int)id];   // table is source of truth
    const Gauge& g = gs.g[(int)id];
    Zone z = zoneForStat(gs, (int)id);         // thresholds from the table (OIL P gated engine-off)

    // Stat name — white in day mode, amber in night mode (consistent with quad cells).
    lv_label_set_text(fName, statLabel((int)id));   // display text, not the log key
    lv_obj_set_style_text_color(fName, (theme==Theme::Day) ? lv_color_white()
                                                            : lv_color_hex(0xFF7A00), 0);
    updateSdLabel(fSd, theme);

    // Large value with unit, zone-coloured. "--" grey if no reading yet.
    char buf[24];
    if (!g.valid) {
      // "SET UP" when the row is waiting on a setting, not on the truck — same
      // rule as the quad cells above.
      snprintf(buf, sizeof buf, g.needsSetup ? "SET UP" : "--");
      lv_label_set_text(fVal, buf);
      lv_obj_set_style_text_color(fVal,
          g.needsSetup ? lv_color_hex(0xFF7A00) : lv_color_hex(0x666666), 0);
    } else {
      snprintf(buf, sizeof buf, (r.decimals ? "%.1f%s" : "%.0f%s"),
               toDisplayValue(r.quantity, g.value, metric), displayUnit(r, metric));
      lv_label_set_text(fVal, buf);
      // Grey when stale (held value, dead PID) — same convention as the quad cells.
      lv_obj_set_style_text_color(fVal,
        g.stale ? lv_color_hex(0x666666) : zoneColor(z, theme), 0);
    }

    // --- 5-minute trend graph (auto-fit to the data's own min/max) ---
    // Per-segment colour is applied in focusChartDrawCb from each sample's zone;
    // publish this stat's thresholds + theme so the callback can compute them.
    g_focusThr   = r.thr;          // thresholds from the table (not gs.t)
    g_focusTheme = theme;
    g_focusQty    = r.quantity;
    g_focusMetric = metric;

    const HistoryRing& ring = hist.ring[(int)id];
    float lo = 0, hi = 1;
    bool have = ring.minmax(&lo, &hi);
    // Pad a flat/near-flat trace so it sits mid-height instead of pinned to an edge.
    if (!have) { lo = g.value - 1; hi = g.value + 1; }
    if (hi - lo < 1e-3f) { float mid = (hi + lo) * 0.5f; lo = mid - 0.5f; hi = mid + 0.5f; }
    const float span = hi - lo;
    // Publish the current scale so the axis-label draw callback maps Y ticks to
    // real values (and uses the right decimal precision for this stat).
    g_focusLo = lo; g_focusHi = hi; g_focusDec = r.decimals;

    // Manual Y-axis value labels (top→bottom: hi, +2/3, +1/3, lo).
    const float yVal[4] = { hi, lo + span * 2.0f / 3.0f, lo + span / 3.0f, lo };
    for (int i = 0; i < 4; i++) {
      char yb[12];
      snprintf(yb, sizeof yb, r.decimals ? "%.1f" : "%.0f",
               (double)toDisplayValue(r.quantity, yVal[i], metric));
      lv_label_set_text(fYLabel[i], yb);
    }

    // Rebuild the series only when the focused stat changes or a new sample has
    // been pushed (history advances at 1 Hz — track the sample clock, NOT count,
    // which saturates at HISTORY_LEN and would freeze the graph after 5 min).
    static StatId   lastId  = (StatId)-1;
    static uint32_t lastSmp = 0xFFFFFFFFu;
    if (id != lastId || hist.lastSampleMs != lastSmp) {
      lastId = id; lastSmp = hist.lastSampleMs;
      lv_coord_t* ya = lv_chart_get_y_array(fChart, fSeries);
      const int N = HISTORY_LEN;
      // Right-align: newest sample at the right edge; empties (NONE) on the left.
      for (int idx = 0; idx < N; idx++) {
        int fromRight = N - 1 - idx;
        int s = ring.count - 1 - fromRight;   // sample index for this column
        if (s < 0) { ya[idx] = LV_CHART_POINT_NONE; continue; }
        float norm = (ring.get(s) - lo) / span * 1000.0f;
        if (norm < 0) norm = 0; if (norm > 1000) norm = 1000;
        ya[idx] = (lv_coord_t)norm;
      }
      lv_chart_refresh(fChart);
    }

    // Peak (bottom-left). The graph's Y axis now shows the value scale and the
    // X axis the time window, so no separate range label is needed.
    char pk[28];
    snprintf(pk, sizeof pk, (r.decimals ? "Peak: %.1f%s" : "Peak: %.0f%s"),
             toDisplayValue(r.quantity, g.peak, metric), displayUnit(r, metric));
    lv_label_set_text(fPeak, pk);
    lv_obj_set_style_text_color(fPeak, lv_color_hex(0xAAAAAA), 0);
  }

  // --- Alarm overlay (runs every frame, regardless of which screen is active) ---
  // Collect hold-off-confirmed alarms, worst-first (every Red, then every Amber,
  // each group in StatId order) and list them as a full-screen takeover, cascading
  // top-down (1st on top). No blinking — static and steady.
  // A stat must be continuously non-Green for >= 4 s before its alarm appears;
  // this eliminates nuisance triggers during crank/start-stop voltage dips.
  {
    uint32_t now = lv_tick_get();
    // Startup grace: hide the overlay and skip alarm evaluation entirely, so the
    // hold-off re-arms fresh after grace ends (a sustained fault then needs a new
    // 4s window before showing). This block is the last thing in render().
    if (g_suppressAlarms) {
      g_anyAlarm = false;
      g_alarmShown = false;
      lv_obj_add_flag(alarmOverlay, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    bool hot[READOUT_COUNT] = {false};       // confirmed-alarming this frame
    // Phase 1: run every displayed stat through the hold-off so greens reset timers.
    for (int i = 0; i < READOUT_COUNT; i++) {
      if (!isDisplayed(i)) continue;         // skip hidden helpers / deactivated
      // Stale (held, dead PID) values are frozen data — treat as Green so they can
      // neither trigger a new alarm nor keep one latched forever.
      Zone z = (gs.g[i].valid && !gs.g[i].stale)
                 ? zoneForStat(gs, i) : Zone::Green;
      hot[i] = alarmHold.confirmed(i, z, now);
    }
    int order[STAT_COUNT];
    int nAlarm = 0;
    // Phase 2: collect worst-first, gated by the hold-off flag.
    // pass 2 = Red, pass 1 = Amber (Zone enum: Green=0, Amber=1, Red=2).
    for (int pass = 2; pass >= 1; pass--)
      for (int i = 0; i < READOUT_COUNT; i++)
        if (hot[i] && (int)zoneForStat(gs, i) == pass) order[nAlarm++] = i;
    bool anyActive = (nAlarm > 0);
    g_anyAlarm = anyActive;                       // expose to anyAlarm()
    // Show unless the user has acked it (acked alarms stay hidden until all
    // clear, then a fresh one re-shows).
    bool show = g_alarmAck.shouldShow(anyActive);
    g_alarmShown = show;

    if (show) {
      lv_obj_clear_flag(alarmOverlay, LV_OBJ_FLAG_HIDDEN);   // cover the whole screen
      for (int i = 0; i < STAT_COUNT; i++) {
        if (i < nAlarm) {
          int s = order[i];
          Zone z = zoneFor(gs.g[s].value, READOUTS[s].thr);  // thresholds from the table
          char b[48];
          snprintf(b, sizeof b, "%s %s %.0f%s",
                   statLabel(s), z == Zone::Red ? "CRITICAL" : "WARN",
                   toDisplayValue(READOUTS[s].quantity, gs.g[s].value, metric),
                   displayUnit(READOUTS[s], metric));
          lv_label_set_text(alarmLine[i], b);
          lv_obj_set_style_text_color(alarmLine[i],
            z == Zone::Red ? lv_color_hex(0xFF3030) : lv_color_hex(0xFFB000), 0);
          lv_obj_clear_flag(alarmLine[i], LV_OBJ_FLAG_HIDDEN);
        } else {
          lv_obj_add_flag(alarmLine[i], LV_OBJ_FLAG_HIDDEN);  // unused slots
        }
      }
    } else {
      lv_obj_add_flag(alarmOverlay, LV_OBJ_FLAG_HIDDEN);      // no alarms — show gauges
    }
  }
}

}  // namespace ui
