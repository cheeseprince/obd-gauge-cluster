// Display HAL for the CrowPanel Advance 3.5" (ESP32-S3) — LovyanGFX backend.
// Implements the display:: interface (src/display.h), so
// ui.cpp/main.cpp are unchanged. Selected per-env via build_src_filter.
//   - ILI9488 480x320 IPS, setRotation(0) = upright landscape (confirmed).
//   - LVGL draw buffers in PSRAM (full-screen double buffer, like Elecrow's ref).
//   - Flush via pushImageDMA. Backlight PWM on GPIO38 (Arduino-ESP32 core 3.x ledc).
//   - No touch: this board's input is the Modulino encoder.
#include <Arduino.h>
#include <lvgl.h>
#include "display.h"
#include "lgfx_crowpanel.h"

static LGFX lcd;
static const int W = 480, H = 320;
static const int PIN_BL  = 38;
static const int BL_CH   = 0;      // LEDC channel
static const int BL_FREQ = 5000;   // 5 kHz
static const int BL_RES  = 8;      // 8-bit (0..255)

static lv_disp_draw_buf_t draw_buf;
static lv_color_t* lv_buf0 = nullptr;
static lv_color_t* lv_buf1 = nullptr;

// LVGL flush — push the dirty rect to the panel. rgb565 matches LV_COLOR_DEPTH 16
// + LV_COLOR_16_SWAP 0; LovyanGFX handles the panel byte order for rgb565_t.
static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
  const int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  lcd.pushImageDMA(a->x1, a->y1, w, h, (lgfx::rgb565_t*)&px->full);
  lv_disp_flush_ready(d);
}

namespace display {

void begin() {
  // Backlight PWM (LEDC, channel-based API).
  ledcSetup(BL_CH, BL_FREQ, BL_RES);
  ledcAttachPin(PIN_BL, BL_CH);
  ledcWrite(BL_CH, 255);               // full bright at boot

  lcd.init();
  lcd.setRotation(0);                  // upright landscape 480x320 (confirmed on HW)
  lcd.startWrite();                    // hold the SPI bus (display-only; no SD use)

  lv_init();
  const size_t n = (size_t)W * H;      // full-screen double buffer in PSRAM
  lv_buf0 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_buf1 = (lv_color_t*)heap_caps_malloc(n * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!lv_buf0 || !lv_buf1) {
    Serial.println("[DISPLAY] FATAL: PSRAM alloc for LVGL buffers failed");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  lv_disp_draw_buf_init(&draw_buf, lv_buf0, lv_buf1, n);

  static lv_disp_drv_t ddrv;
  lv_disp_drv_init(&ddrv);
  ddrv.hor_res  = W;
  ddrv.ver_res  = H;
  ddrv.flush_cb = flush_cb;
  ddrv.draw_buf = &draw_buf;
  lv_disp_drv_register(&ddrv);
  // No touch indev — navigation is the Modulino encoder (encoder_input.cpp).
}

void setBacklight(uint8_t pct) {
  if (pct > 100) pct = 100;
  ledcWrite(BL_CH, (pct * 255) / 100);
}

void tick() { lv_timer_handler(); }

// No touch hardware on this board — report "not pressed".
bool touch(int16_t* x, int16_t* y) { *x = 0; *y = 0; return false; }


}  // namespace display
