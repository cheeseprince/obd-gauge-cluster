#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include "display.h"

static TFT_eSPI tft = TFT_eSPI();
static const int W = 480, H = 320;   // explicit constants — do NOT use TFT_WIDTH/TFT_HEIGHT
                                       // (TFT_eSPI's ILI9488_Defines.h redefines them to portrait)
static lv_disp_draw_buf_t draw_buf;
static const int BUF_LINES = 40;      // partial buffer: 40 lines × 480 px
// MOCK_OBD (no BT): static DRAM buffer — full DRAM budget available (no BT stack overhead).
// Real-OBD build: BT stack reserves ~56KB of internal DRAM (CONFIG_BTDM_RESERVE_DRAM=0xdb5c).
//   Allocate the 38KB LVGL buffer from PSRAM heap at begin() time so it doesn't consume
//   static BSS in dram0_0_seg. BOARD_HAS_PSRAM (set in platformio.ini elecrow_obd build_flags)
//   enables psramInit() and ps_malloc() for this path.
#ifdef MOCK_OBD
static DRAM_ATTR lv_color_t lv_buf[W * BUF_LINES];
#else
static lv_color_t* lv_buf = nullptr;  // allocated from PSRAM in begin()
#endif

// Touch calibration array for THIS panel.
// Calibrated on our unit 2026-06-18 with a stylus (press 'c' in serial to re-run).
// Order: {x0, x1, y0, y1, rotation_flags}
static uint16_t touchCal[5] = {288, 3625, 227, 3568, 7};

// Backlight PWM via LEDC — GPIO27 is dimmable even though Elecrow only documents on/off.
static const int BL_CH   = 0;      // LEDC channel 0
static const int BL_FREQ = 5000;   // 5 kHz PWM frequency
static const int BL_RES  = 8;      // 8-bit resolution (0–255)

// LVGL flush callback — sends a filled rectangle to the ILI9488.
// pushColors(..., true) performs the byte-swap needed with LV_COLOR_16_SWAP 0;
// colors verified correct on real hardware in Task 1 — do NOT change.
static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
  int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t*)px, w * h, true);   // true = byte-swap; pairs with LV_COLOR_16_SWAP 0
  tft.endWrite();
  lv_disp_flush_ready(d);
}

// Cached latest touch state (set by touch_cb, read by display::touch()).
static volatile bool    g_touchDown = false;
static volatile int16_t g_touchX = 0, g_touchY = 0;

// LVGL touch indev callback.
// We deliberately do NOT use tft.getTouch(): its dual-sample validation rejects
// a MOVING finger (the two position samples differ), so it never reports a press
// during a drag — which kills LVGL swipe-gesture detection and makes taps flaky.
// Instead: detect press from raw pressure (single, continuous read that tracks
// through a drag) and map raw ADC -> screen with a direct linear calibration
// measured on our unit. Press threshold 400: real presses read Z=1400+, noise ~210.
static void touch_cb(lv_indev_drv_t* /*drv*/, lv_indev_data_t* data) {
  static bool pressed = false;
  static int16_t lastX = 0, lastY = 0;
  uint16_t z = tft.getTouchRawZ();

  // Hysteresis on the press state: ENTER at z>400 (well above ~210 noise), but
  // STAY pressed until z<120. A moving swipe makes contact pressure dip; without
  // hysteresis the press flickers off mid-drag and LVGL can't accumulate a
  // gesture. Latching keeps one continuous press from finger-down to finger-up.
  if (!pressed) { if (z > 400) pressed = true; }
  else          { if (z < 120) pressed = false; }

  if (pressed) {
    uint16_t rx, ry;
    tft.getTouchRaw(&rx, &ry);
    // Measured corners (raw -> screen) on our panel, rotation 1:
    //   top-left  raw(rx=3725, ry=3875) -> (0,0)
    //   bot-right raw(rx= 328, ry= 370) -> (479,319)
    // AXES ARE SWAPPED: screen X comes from raw_Y, screen Y from raw_X (confirmed
    // by TR/BL taps swapping Oil<->Boost). Both axes also inverted.
    //   screen_x = (3875 - ry) * 480 / 3505
    //   screen_y = (3725 - rx) * 320 / 3397
    int sx = (3875 - (int)ry) * 480 / 3505;
    int sy = (3725 - (int)rx) * 320 / 3397;
    sx = sx < 0 ? 0 : (sx > 479 ? 479 : sx);
    sy = sy < 0 ? 0 : (sy > 319 ? 319 : sy);
    lastX = sx; lastY = sy;
    data->state   = LV_INDEV_STATE_PRESSED;
    data->point.x = sx;
    data->point.y = sy;
  } else {
    data->state   = LV_INDEV_STATE_RELEASED;
    data->point.x = lastX;   // hold last point so LVGL can complete a click
    data->point.y = lastY;
  }

  // Cache for the app's own tap/swipe detection (display::touch()).
  g_touchDown = pressed;
  g_touchX = lastX;
  g_touchY = lastY;
}

namespace display {

void begin() {
  // --- Backlight: LEDC PWM, full bright at boot ---
  ledcSetup(BL_CH, BL_FREQ, BL_RES);
  ledcAttachPin(TFT_BL, BL_CH);
  ledcWrite(BL_CH, 255);   // 100 % = 255 / 255

  // --- TFT hardware init ---
  tft.init();
  tft.setRotation(1);          // landscape — verified correct on real hardware (Task 1)
  tft.setTouch(touchCal);      // load calibration into XPT2046 driver

  // --- LVGL init ---
  lv_init();
#ifndef MOCK_OBD
  // Real-OBD build: allocate draw buffer from PSRAM to keep internal DRAM free for BT.
  lv_buf = (lv_color_t*)ps_malloc(W * BUF_LINES * sizeof(lv_color_t));
  if (!lv_buf) {
    Serial.println("[DISPLAY] FATAL: PSRAM alloc for lv_buf failed");
    // Halt — nothing useful can happen without a draw buffer.
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif
  lv_disp_draw_buf_init(&draw_buf, lv_buf, nullptr, W * BUF_LINES);

  static lv_disp_drv_t ddrv;
  lv_disp_drv_init(&ddrv);
  ddrv.hor_res  = W;
  ddrv.ver_res  = H;
  ddrv.flush_cb = flush_cb;
  ddrv.draw_buf = &draw_buf;
  lv_disp_drv_register(&ddrv);

  // --- Touch indev ---
  static lv_indev_drv_t idrv;
  lv_indev_drv_init(&idrv);
  idrv.type    = LV_INDEV_TYPE_POINTER;
  idrv.read_cb = touch_cb;
  lv_indev_drv_register(&idrv);
}

void setBacklight(uint8_t pct) {
  if (pct > 100) pct = 100;
  ledcWrite(BL_CH, (pct * 255) / 100);
}

void tick() { lv_timer_handler(); }

bool touch(int16_t* x, int16_t* y) {
  *x = g_touchX;
  *y = g_touchY;
  return g_touchDown;
}

void calibrateTouch() {
  uint16_t cal[5];
  tft.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 20);  // follow the corner prompts on screen
  Serial.printf("touchCal = {%u, %u, %u, %u, %u}  <-- send these to Claude to bake into display_esp32.cpp\n",
                cal[0], cal[1], cal[2], cal[3], cal[4]);
  tft.setTouch(cal);  // apply for the rest of this session so you can verify immediately
}

}  // namespace display
