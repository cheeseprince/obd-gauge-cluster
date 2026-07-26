#include "wifi_cred_store.h"
#include <cstring>
#include <cstdio>

static void copyBounded(char* dst, size_t dstN, const char* src) {
  snprintf(dst, dstN, "%s", src ? src : "");
}

int credFind(const WifiCredList& l, const char* ssid) {
  if (!ssid) return -1;
  for (int i = 0; i < l.n; i++)
    if (strncmp(l.c[i].ssid, ssid, sizeof l.c[i].ssid) == 0) return i;
  return -1;
}

bool credAdd(WifiCredList& l, const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return false;
  int i = credFind(l, ssid);
  if (i < 0) {
    if (l.n >= WIFI_CRED_MAX) return false;   // full and new -> caller must delete one
    i = l.n++;
    copyBounded(l.c[i].ssid, sizeof l.c[i].ssid, ssid);
  }
  copyBounded(l.c[i].pass, sizeof l.c[i].pass, pass);   // same SSID = update password
  return true;
}

bool credRemove(WifiCredList& l, int idx) {
  if (idx < 0 || idx >= l.n) return false;
  for (int i = idx; i < l.n - 1; i++) l.c[i] = l.c[i + 1];
  l.n--;
  return true;
}

#ifdef ARDUINO
#include <Preferences.h>

// NVS layout: namespace "wifi", key "n" (count) + "s0..s4"/"p0..p4".
void credLoad(WifiCredList& l) {
  Preferences p;
  p.begin("wifi", true);
  l.n = 0;
  int n = p.getUChar("n", 0);
  if (n > WIFI_CRED_MAX) n = WIFI_CRED_MAX;
  char sk[4] = "s0", pk[4] = "p0";
  for (int i = 0; i < n; i++) {
    sk[1] = pk[1] = (char)('0' + i);
    String s = p.getString(sk, ""), pw = p.getString(pk, "");
    if (s.length()) credAdd(l, s.c_str(), pw.c_str());
  }
  p.end();
}

void credSave(const WifiCredList& l) {
  Preferences p;
  p.begin("wifi", false);
  p.clear();                          // drop stale higher-index keys on shrink
  p.putUChar("n", (uint8_t)l.n);
  char sk[4] = "s0", pk[4] = "p0";
  for (int i = 0; i < l.n; i++) {
    sk[1] = pk[1] = (char)('0' + i);
    p.putString(sk, l.c[i].ssid);
    p.putString(pk, l.c[i].pass);
  }
  p.end();
}
#endif  // ARDUINO
