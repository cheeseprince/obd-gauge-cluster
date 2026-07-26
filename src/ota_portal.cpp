#include "ota_portal.h"

#if HAS_OTA

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include "wifi_cred_store.h"
#include "solar.h"

// SoftAP password — random per DEVICE, generated on the first WiFi-setup run
// and persisted in NVS; shown on the device screen while the portal runs so
// it never needs to be known in advance. Replaces the old compile-time
// constant: it was identical on every unit AND extractable from the published
// .bin with `strings`. Not derived from the eFuse MAC either —
// the derivation would be public source and the MAC is on the air (the AP's
// BSSID). STABLE across sessions on purpose: the SSID is stable, so a
// rotating password would strand any phone that saved the network (auto-join
// with the stale PSK fails; iOS then wants "Forget This Network" every
// single session). 8 hex chars keeps WPA2's minimum passphrase length.
static const char* apPass() {
  static char pw[9] = {0};
  if (!pw[0]) {
    Preferences p; p.begin("obd", false);
    String s = p.getString("appass", "");
    if (s.length() == 8) {
      snprintf(pw, sizeof pw, "%s", s.c_str());
    } else {
      snprintf(pw, sizeof pw, "%08lx", (unsigned long)esp_random());
      p.putString("appass", pw);
    }
    p.end();
  }
  return pw;
}
// Per-session CSRF token, required on EVERY state-changing endpoint (/add,
// /del, /loc, /rescan, /exit): a cross-site page a victim opens while on the
// setup AP can't forge it (it's random and only ever appears in this
// session's own pages — as a hidden field in the POST forms and as a query
// parameter on the GET links).
static char g_csrf[9] = {0};

// SoftAP SSID = "OBD-XXXX" where XXXX is 4 hex digits derived from the chip's
// MAC — unique per device (so multiple units nearby don't collide), stable
// across reboots (the same AP name appears every time), and shorter than the
// old fixed "OBD-CLUSTER-SETUP". Built once on first use.
static const char* apSsid() {
  static char ssid[12] = {0};
  if (!ssid[0])
    snprintf(ssid, sizeof ssid, "OBD-%04X", (unsigned)(ESP.getEfuseMac() & 0xFFFF));
  return ssid;
}

// String::toFloat()/toInt() return 0 for non-numeric input (a lone "-", or a
// stray letter from the full text keyboard), which would pass the numeric range
// checks and silently save (0,0)/UTC+0 (Null Island). Require each location
// field to actually parse as a number: optional sign, digits, at most one dot,
// and at least one digit.
static bool isNumericField(const String& s) {
  int n = s.length();
  if (n == 0) return false;
  int i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
  bool digit = false, dot = false;
  for (; i < n; i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') digit = true;
    else if (c == '.' && !dot) dot = true;
    else return false;
  }
  return digit;
}

// HTML-escape untrusted text (scanned/saved SSIDs) before placing it in the
// portal page — a crafted SSID must not be able to inject markup or script.
static String htmlEscape(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  o += "&amp;";  break;
      case '<':  o += "&lt;";   break;
      case '>':  o += "&gt;";   break;
      case '"':  o += "&quot;"; break;
      case '\'': o += "&#39;";  break;
      default:   o += c;
    }
  }
  return o;
}

static WebServer*   g_srv  = nullptr;
static DNSServer*   g_dns  = nullptr;
static WifiCredList g_list;
static bool         g_done = false;
static String       g_scanHtml;   // cached scan results (rebuilt on /rescan)

// Rebuild the scanned-networks section (called at start and on /rescan).
// Synchronous scan (~2s) is fine here — the portal is a dedicated mode.
static void rescan() {
  int n = WiFi.scanNetworks();
  g_scanHtml = "";
  for (int i = 0; i < n && i < 15; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    // HTML-escape the SSID (untrusted radio input) and pass it to the prefill
    // via a data-attribute + a FIXED handler, so the SSID never enters inline JS
    // source — a crafted SSID cannot inject markup or script into the page.
    String e = htmlEscape(s);
    g_scanHtml += "<li><a href='#' data-ssid='" + e +
                  "' onclick=\"document.f.ssid.value=this.dataset.ssid;return false\">" + e +
                  "</a> (" + String(WiFi.RSSI(i)) + " dBm)" +
                  (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " open" : "") + "</li>";
  }
  WiFi.scanDelete();
}

static void sendPage() {
  String h =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>OBD Cluster WiFi</title><style>body{font-family:sans-serif;margin:1em;max-width:30em}"
    "li{margin:.3em 0}input{font-size:1.1em;width:100%;margin:.2em 0;padding:.3em}"
    "button{font-size:1.1em;padding:.4em 1em;margin:.3em .3em .3em 0}</style></head><body>"
    "<h2>OBD Cluster &mdash; WiFi setup</h2><h3>Saved networks (" + String(g_list.n) + "/" +
    String(WIFI_CRED_MAX) + ")</h3><ul>";
  for (int i = 0; i < g_list.n; i++)
    h += "<li>" + htmlEscape(String(g_list.c[i].ssid)) +
         " &mdash; <a href='/del?i=" + String(i) + "&t=" + g_csrf + "'>delete</a></li>";
  if (!g_list.n) h += "<li><i>none yet</i></li>";
  h += "</ul><h3>Nearby networks</h3><ul>" + g_scanHtml + "</ul>"
       "<p><a href='/rescan?t=" + String(g_csrf) + "'>rescan</a></p>"
       // Plain-text password + autocorrect/capitalize OFF: phone keyboards
       // silently mangle hidden password fields (the classic wrong-first-cap),
       // and you can't see it happen. Visible text = you type what you get.
       "<h3>Add network</h3><form name=f method=POST action='/add'>"
       "<input type=hidden name=t value='" + String(g_csrf) + "'>"
       "<input name=ssid placeholder='SSID' maxlength=32 "
       "autocapitalize=off autocorrect=off spellcheck=false autocomplete=off>"
       "<input name=pass type=text placeholder='password (blank if open)' maxlength=64 "
       "autocapitalize=off autocorrect=off spellcheck=false autocomplete=off>"
       "<button type=submit>Save</button></form>";

  // Location (for auto night mode) — read current NVS value to pre-fill;
  // blank fields when unset so a stale 0,0 is never implied.
  GeoLocation cur;
  {
    Preferences p; p.begin("obd", true);
    cur.lat   = p.getFloat("lat", 200.0f);
    cur.lon   = p.getFloat("lon", 0.0f);
    cur.tzStd = p.getChar("tz", 0);
    p.end();
  }
  char latBuf[16] = "", lonBuf[16] = "", tzBuf[8] = "";
  if (geoValid(cur)) {
    snprintf(latBuf, sizeof latBuf, "%.4f", cur.lat);
    snprintf(lonBuf, sizeof lonBuf, "%.4f", cur.lon);
    snprintf(tzBuf,  sizeof tzBuf,  "%d",   cur.tzStd);
  }
  // Placeholder examples must stay a neutral location (New York), never the
  // developer's own coordinates — this form ships compiled into the binary
  // and is shown to every user who opens the setup page.
  h += "<h3>Location (for auto night mode)</h3>"
       "<form method=POST action='/loc'>"
       "<input type=hidden name=t value='" + String(g_csrf) + "'>"
       // inputmode=text (not decimal): iOS's decimal keypad has NO minus key, so
       // a western longitude (-121) or a negative UTC offset (-8) is impossible to
       // enter. The full text keyboard exposes the minus (on its 123 page). Same
       // reason for latitude (southern hemisphere is negative).
       "<input name=lat inputmode=text placeholder='latitude (e.g. 40.71)' "
       "value='" + String(latBuf) + "' autocapitalize=off autocorrect=off spellcheck=false>"
       "<input name=lon inputmode=text placeholder='longitude, west is negative (e.g. -74.01)' "
       "value='" + String(lonBuf) + "' autocapitalize=off autocorrect=off spellcheck=false>"
       "<input name=tz inputmode=text placeholder='UTC offset (e.g. -5)' "
       "value='" + String(tzBuf) + "' autocapitalize=off autocorrect=off spellcheck=false>"
       "<button type=submit>Save location</button></form>"
       "<p>West longitude is negative. On iOS, tap <b>123</b> on the keyboard to "
       "reach the minus sign. US daylight saving is applied automatically.</p>";

  h += "<hr><form action='/exit'><input type=hidden name=t value='" + String(g_csrf) +
       "'><button type=submit>Done &mdash; reboot display</button></form>"
       "</body></html>";
  g_srv->send(200, "text/html", h);
}

static void redirectHome() {
  g_srv->sendHeader("Location", "/", true);
  g_srv->send(302, "text/plain", "");
}

void otaPortalRun(void (*pump)(const char* status)) {
  credLoad(g_list);
  g_done = false;
  snprintf(g_csrf, sizeof g_csrf, "%08lX", (unsigned long)esp_random());   // fresh CSRF token

  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);         // STA side kept up so scanNetworks works
  WiFi.softAP(apSsid(), apPass());
  rescan();

  WebServer srv(80);
  DNSServer dns;
  g_srv = &srv; g_dns = &dns;
  dns.start(53, "*", WiFi.softAPIP());   // captive: every hostname -> us

  srv.on("/", sendPage);
  srv.on("/add", HTTP_POST, []() {
    if (g_srv->arg("t") != g_csrf) { redirectHome(); return; }   // CSRF: reject a forged request
    credAdd(g_list, g_srv->arg("ssid").c_str(), g_srv->arg("pass").c_str());
    credSave(g_list);
    redirectHome();
  });
  srv.on("/del", []() {
    if (g_srv->arg("t") != g_csrf) { redirectHome(); return; }   // CSRF: reject a forged request
    credRemove(g_list, g_srv->arg("i").toInt());
    credSave(g_list);
    redirectHome();
  });
  srv.on("/rescan", []() {
    if (g_srv->arg("t") != g_csrf) { redirectHome(); return; }   // CSRF: reject a forged request
    rescan(); redirectHome();
  });
  srv.on("/loc", HTTP_POST, []() {
    if (g_srv->arg("t") != g_csrf) { redirectHome(); return; }   // CSRF: reject a forged request
    float lat = g_srv->arg("lat").toFloat();
    float lon = g_srv->arg("lon").toFloat();
    int   tz  = g_srv->arg("tz").toInt();
    // All three fields required AND must be genuinely numeric (not garbage that
    // toFloat/toInt silently coerce to 0 -> a bogus but in-range 0,0/UTC+0 save).
    bool ok = isNumericField(g_srv->arg("lat")) &&
              isNumericField(g_srv->arg("lon")) &&
              isNumericField(g_srv->arg("tz")) &&
              lat >= -90.0f && lat <= 90.0f &&
              lon >= -180.0f && lon <= 180.0f &&
              tz  >= -12 && tz <= 14;
    if (ok) {
      Preferences p; p.begin("obd", false);
      p.putFloat("lat", lat);
      p.putFloat("lon", lon);
      p.putChar("tz", (int8_t)tz);
      p.end();
      // Offer both a Done button (finish here) and Back (keep configuring) —
      // previously this page only had a Back link, so finishing meant Back->Done.
      g_srv->send(200, "text/html",
        String("<p>Location saved.</p>"
        "<form action='/exit'><input type=hidden name=t value='") + g_csrf +
        "'><button type=submit>Done &mdash; reboot display</button></form>"
        "<p><a href='/'>Back to setup</a></p>");
    } else {
      g_srv->send(200, "text/html",
        "<p>Invalid values &mdash; latitude -90..90, longitude -180..180, "
        "UTC offset -12..14, all three required. <a href='/'>Back</a></p>");
    }
  });
  srv.on("/exit", []() {
    if (g_srv->arg("t") != g_csrf) { redirectHome(); return; }   // CSRF: reject a forged request
    g_srv->send(200, "text/html", "<h2>Saved. Display rebooting&hellip;</h2>");
    g_done = true;
  });
  srv.onNotFound(redirectHome);   // captive-portal probes land on the page
  srv.begin();

  char status[128];
  uint32_t t0 = millis(), lastPump = 0;
  while (!g_done && (int32_t)(millis() - (t0 + 300000)) < 0) {   // 5 min cap
    dns.processNextRequest();
    srv.handleClient();
    if ((int32_t)(millis() - lastPump) > 250) {                  // ~4 Hz UI refresh
      lastPump = millis();
      snprintf(status, sizeof status,
               "WiFi setup\n\nJoin WiFi: %s\nPassword: %s\nOpen http://%s\n\nSaved: %d/%d\n(reboots when done)",
               apSsid(), apPass(), WiFi.softAPIP().toString().c_str(),
               g_list.n, WIFI_CRED_MAX);
      pump(status);
    }
    delay(2);
  }
  srv.stop();
  dns.stop();
  g_srv = nullptr; g_dns = nullptr;
  pump("WiFi setup done\nrebooting...");
  delay(800);                     // let the phone render the goodbye page
}

#endif  // HAS_OTA
