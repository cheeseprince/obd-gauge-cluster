#include "sd_log.h"
#include "readouts.h"
#include "vehicle_active.h"
#include <cstdio>
#include <cstdarg>

// Append to out[0..n) starting at *p, guarding the buffer. Advances *p.
static void appendf(char* out, int n, int* p, const char* fmt, ...) {
  if (*p >= n) return;
  va_list ap; va_start(ap, fmt);
  *p += vsnprintf(out + *p, n - *p, fmt, ap);
  va_end(ap);
}

int buildHeaderRow(char* out, int n) {
  int p = 0;
  appendf(out, n, &p, "datetime,uptime_s");
  for (int i = 0; i < STAT_COUNT; i++)
    if (isActive(i)) appendf(out, n, &p, ",%s", READOUTS[i].name);
  appendf(out, n, &p, "\n");
  return p < n ? p : n - 1;
}

int buildDataRow(const DateTime& now, uint32_t uptimeS,
                 const float* v, const bool* valid, char* out, int n) {
  char dt[24]; formatDateTime(now, dt, sizeof dt);
  int p = 0;
  appendf(out, n, &p, "%s,%lu", dt, (unsigned long)uptimeS);
  for (int i = 0; i < STAT_COUNT; i++) {
    if (!isActive(i)) continue;
    if (valid[i]) appendf(out, n, &p, READOUTS[i].decimals ? ",%.1f" : ",%.0f", (double)v[i]);
    else          appendf(out, n, &p, ",");
  }
  appendf(out, n, &p, "\n");
  return p < n ? p : n - 1;
}

int buildLogPath(const DateTime& now, bool rtcValid, unsigned session, char* out, int n) {
  int p = 0;
  if (rtcValid && now.y >= 2000) {
    char stamp[20]; formatStamp(now, stamp, sizeof stamp);
    appendf(out, n, &p, "/logs/%s.csv", stamp);
  } else {                                   // RTC never read -> NVS session counter
    appendf(out, n, &p, "/logs/sess%03u.csv", session);
  }
  return p < n ? p : n - 1;
}

#ifdef ARDUINO
#include <SPI.h>
#include <SD.h>
#include <Preferences.h>

// microSD on the dedicated HSPI bus (CrowPanel Advance 3.5). CS=IO7 = confirm vs schematic.
#define SD_SCK  5
#define SD_MISO 4
#define SD_MOSI 6
#define SD_CS   7

static SPIClass  s_spi(HSPI);
static LogStatus s_status = LogStatus::NoCard;
static File      s_file;
static bool      s_open   = false;
static uint32_t  s_lastRow = 0;

LogStatus logStatus() { return s_status; }

void sdBegin() {
  s_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, s_spi, 20000000)) {
    SD.mkdir("/logs");
    s_status = LogStatus::Off;   // card present; On/Off set by logTick per enable
    Serial.println("[SD] card OK");
  } else {
    s_status = LogStatus::NoCard;
    Serial.println("[SD] no card / mount failed");
  }
}

static void openFile(const DateTime& now, bool rtcValid) {
  char path[48];
  unsigned sn = 0;
  if (!(rtcValid && now.y >= 2000)) {        // fallback name -> bump the session counter
    Preferences p; p.begin("obd", false);
    sn = p.getUShort("logn", 0) + 1; p.putUShort("logn", (uint16_t)sn); p.end();
  }
  buildLogPath(now, rtcValid, sn, path, sizeof path);
  // APPEND, never FILE_WRITE: ESP32 FILE_WRITE is "w" (truncate), which silently
  // destroys the previous drive's log on any filename collision (e.g. RTC stuck).
  // Append preserves existing rows; the header is written only for a new file.
  s_file = SD.open(path, FILE_APPEND);
  if (!s_file) { s_status = LogStatus::Err; return; }
  if (s_file.size() == 0) {
    char hdr[512]; int len = buildHeaderRow(hdr, sizeof hdr);
    if (s_file.write((const uint8_t*)hdr, len) != (size_t)len) { s_file.close(); s_status = LogStatus::Err; return; }
    s_file.flush();
  }
  s_open = true; s_status = LogStatus::On; s_lastRow = 0;
  Serial.printf("[SD] logging %s\n", path);
}

static void closeFile() {
  if (s_open) { s_file.flush(); s_file.close(); s_open = false; Serial.println("[SD] file closed"); }
}

void logTick(bool linked, bool enabled, const DateTime& now, bool rtcValid,
             uint32_t nowMs, const float* v, const bool* valid) {
  // NoCard/Err recovery: retry a full remount every 30 s instead of latching until
  // power cycle — covers a card inserted after boot, pulled + reinserted with the
  // key on (the documented way to read logs), and transient write errors on the
  // switched-power SPI bus. Success drops to Off; the normal flow below reopens.
  if (s_status == LogStatus::NoCard || s_status == LogStatus::Err) {
    static uint32_t lastRetry = 0;
    if (nowMs - lastRetry < 30000) return;
    lastRetry = nowMs;
    closeFile();
    SD.end();
    if (SD.begin(SD_CS, s_spi, 20000000)) {
      SD.mkdir("/logs");
      s_status = LogStatus::Off;
      Serial.println("[SD] card (re)mounted");
    } else {
      s_status = LogStatus::NoCard;
    }
    return;
  }
  if (!enabled) { closeFile(); s_status = LogStatus::Off; return; }
  if (!linked)  { closeFile(); s_status = LogStatus::Off; return; }
  if (!s_open) openFile(now, rtcValid);                  // open on link-up
  if (!s_open) return;                                   // open failed -> Err; retried above
  if (nowMs - s_lastRow < 1000) return;                  // 1 Hz
  s_lastRow = nowMs;
  char row[512]; int len = buildDataRow(now, nowMs / 1000, v, valid, row, sizeof row);
  if (s_file.write((const uint8_t*)row, len) != (size_t)len) { closeFile(); s_status = LogStatus::Err; return; }
  s_file.flush();                                         // sync each row (power-loss safety)
}
#endif
