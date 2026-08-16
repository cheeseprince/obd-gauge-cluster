// Two pure functions that had real consequences and no host coverage until
// 2026-08-16. Both were `static` inside a .cpp that no test links, so the
// claims made about them in the security audit and in code comments were
// verified by reading only.
//
//   htmlEscape   the F-03 XSS control. Its input is a WiFi SSID, which anyone
//                in radio range chooses.
//   parseSemver  decides whether an OTA update installs at all.
#include <cassert>
#include <cstdio>
#include <string>
#include "../src/html_escape.h"
#include "../src/semver.h"

static int failures = 0;
static void check(bool c, const char* m) { if (!c) { std::printf("FAIL: %s\n", m); failures++; } }
static std::string esc(const std::string& s) { return htmlEscape(s); }

int main() {
  // ---- htmlEscape: all five, and why each one matters -------------------
  check(esc("plain") == "plain", "escape: ordinary text is untouched");
  check(esc("a&b")  == "a&amp;b",  "escape: &");
  check(esc("a<b")  == "a&lt;b",   "escape: <");
  check(esc("a>b")  == "a&gt;b",   "escape: >");
  check(esc("a\"b") == "a&quot;b", "escape: double quote");
  check(esc("a'b")  == "a&#39;b",  "escape: apostrophe");

  // THE APOSTROPHE IS THE ONE THAT MATTERS MOST HERE, and it is the one an
  // earlier version of the portal missed. The SSID is rendered into
  // data-ssid='…' — a SINGLE-quoted attribute — so `'` is the character that
  // closes it early. Escaping only & < > would leave this exploitable.
  check(esc("' onmouseover='alert(1)") == "&#39; onmouseover=&#39;alert(1)",
        "escape: apostrophe cannot break out of a single-quoted attribute");

  // A crafted SSID must not be able to open a tag anywhere in the output.
  {
    const std::string evil = "<script>alert('xss')</script>";
    const std::string out  = esc(evil);
    check(out.find('<') == std::string::npos, "escape: no raw '<' survives");
    check(out.find('>') == std::string::npos, "escape: no raw '>' survives");
    check(out.find('\'') == std::string::npos, "escape: no raw apostrophe survives");
    check(out == "&lt;script&gt;alert(&#39;xss&#39;)&lt;/script&gt;", "escape: full payload");
  }

  // Ampersand must be escaped as itself, not double-escaped, and an SSID that
  // already looks like an entity must not be collapsed back into one.
  check(esc("&amp;") == "&amp;amp;", "escape: an entity-looking SSID is escaped again, not passed through");
  check(esc("") == "", "escape: empty");

  // Real SSIDs contain spaces, dashes and unicode bytes; none are touched.
  check(esc("Alan's Wi-Fi 5G") == "Alan&#39;s Wi-Fi 5G", "escape: realistic SSID");

  // ---- parseSemver: ordering, not string comparison ----------------------
  check(parseSemver("v0.4.7") > parseSemver("v0.4.6"), "semver: patch bump increases");
  check(parseSemver("v0.5.0") > parseSemver("v0.4.99"), "semver: minor outranks patch");
  check(parseSemver("v1.0.0") > parseSemver("v0.99.99"), "semver: major outranks minor");
  check(parseSemver("v0.4.7") == parseSemver("0.4.7"), "semver: leading v is optional");
  check(parseSemver("V0.4.7") == parseSemver("v0.4.7"), "semver: capital V too");

  // The case string sorting gets wrong, which is why a packed integer is used.
  check(parseSemver("v0.10.0") > parseSemver("v0.9.0"),
        "semver: 0.10.0 outranks 0.9.0 (string order would invert this)");

  // A non-release string is OLDEST, so a USB or bench build always updates
  // rather than stranding itself.
  check(parseSemver("local") == 0, "semver: 'local' is oldest");
  check(parseSemver("dev-abc1234") == 0, "semver: dev build is oldest");
  check(parseSemver(nullptr) == 0, "semver: null is oldest");
  check(parseSemver("") == 0, "semver: empty is oldest");
  check(parseSemver("v") == 0, "semver: bare 'v' is oldest");
  check(parseSemver("local") < parseSemver("v0.0.1"), "semver: any release beats a dev build");

  // Parsing stops at the first non-version character, so a suffixed build
  // compares as its release part. This is what made a build stamped
  // "v0.4.4-heapfix" correctly report "Up to date" against v0.4.4 on
  // 2026-08-15 instead of re-downloading itself.
  check(parseSemver("v0.4.4-heapfix") == parseSemver("v0.4.4"), "semver: suffix ignored");
  check(parseSemver("v1.2.3-rc1") == parseSemver("v1.2.3"), "semver: -rc1 does not outrank the release");
  check(parseSemver("v0.4.4-heapfix") < parseSemver("v0.4.5"), "semver: suffixed build still updates");

  // Fields are clamped to 10 bits so one cannot carry into the field above.
  // Without the clamp a huge patch number would outrank a minor bump.
  check(parseSemver("v0.0.9999") == parseSemver("v0.0.1023"), "semver: patch clamps at 1023");
  check(parseSemver("v0.1.0") > parseSemver("v0.0.9999"), "semver: a clamped patch cannot outrank a minor bump");

  // Short forms parse rather than failing closed to 0.
  check(parseSemver("v1") == parseSemver("v1.0.0"), "semver: 'v1' is 1.0.0");
  check(parseSemver("v1.2") == parseSemver("v1.2.0"), "semver: 'v1.2' is 1.2.0");

  if (failures) { std::printf("%d FAILED\n", failures); return 1; }
  std::printf("test_portal_semver: ALL PASS\n");
  return 0;
}
