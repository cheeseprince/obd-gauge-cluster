#pragma once
// CrowPanel Advance 3.5" (ESP32-S3) — ILI9488 480x320 IPS on SPI2_HOST.
// Pins/config lifted from Elecrow's reference LovyanGFX_Driver.h, validated on
// hardware (crowpanel_bringup). Upright landscape = setRotation(0). Touch
// (GT911) omitted — input is the Modulino I2C encoder.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;

 public:
  LGFX(void) {
    { // SPI bus
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 42;
      cfg.pin_mosi = 39;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 41;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    { // panel
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 40;
      cfg.pin_rst  = 2;
      cfg.pin_busy = -1;
      cfg.memory_width    = 320;
      cfg.memory_height   = 480;
      cfg.panel_width     = 320;
      cfg.panel_height    = 480;
      cfg.offset_x        = 0;
      cfg.offset_y        = 0;
      cfg.offset_rotation = 3;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable   = false;
      cfg.invert     = true;
      cfg.rgb_order  = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};
