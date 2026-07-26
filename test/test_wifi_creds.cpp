#include <cstdio>
#include <cstring>
#include "wifi_cred_store.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  WifiCredList l;
  check(l.n == 0, "starts empty");

  // Add + find.
  check(credAdd(l, "HomeGuest", "pass1"), "add first");
  check(credAdd(l, "PhoneAP", "hotspot"), "add second");
  check(l.n == 2, "two stored");
  check(credFind(l, "PhoneAP") == 1, "find by ssid");
  check(credFind(l, "Nope") == -1, "absent -> -1");

  // Same SSID updates the password in place (no duplicate).
  check(credAdd(l, "HomeGuest", "newpass"), "re-add updates");
  check(l.n == 2 && strcmp(l.c[0].pass, "newpass") == 0, "password replaced, no dup");

  // Open network (empty password) is allowed; empty SSID is not.
  check(credAdd(l, "OpenCafe", ""), "open network ok");
  check(!credAdd(l, "", "x"), "empty ssid rejected");

  // Cap at WIFI_CRED_MAX: new SSIDs rejected when full, updates still work.
  credAdd(l, "Net4", "d");
  credAdd(l, "Net5", "e");
  check(l.n == WIFI_CRED_MAX, "list full at max");
  check(!credAdd(l, "Net6", "f"), "new ssid rejected when full");
  check(credAdd(l, "Net5", "e2"), "update still allowed when full");

  // Remove compacts and preserves order.
  check(credRemove(l, 0), "remove head");
  check(l.n == WIFI_CRED_MAX - 1 && strcmp(l.c[0].ssid, "PhoneAP") == 0, "compacted");
  check(!credRemove(l, 99), "oob remove rejected");

  // Truncation: overlong ssid/pass clip, never overflow.
  WifiCredList t;
  char longssid[64]; memset(longssid, 'A', 63); longssid[63] = 0;
  check(credAdd(t, longssid, "p"), "overlong ssid accepted (truncated)");
  check(strlen(t.c[0].ssid) == 32, "ssid clipped to 32");

  printf(failures ? "\n%d FAILED\n" : "\nALL PASS\n", failures);
  return failures ? 1 : 0;
}
