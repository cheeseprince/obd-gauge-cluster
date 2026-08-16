#pragma once

// HTML-escape untrusted text before it is placed in the setup-portal page.
//
// THE UNTRUSTED INPUT IS A WIFI SSID, AND ANYONE IN RANGE CHOOSES IT. The
// portal renders scanned SSIDs into the page twice per row — once inside a
// single-quoted attribute (data-ssid='…') and once as element text — so a
// crafted SSID that escaped this would inject markup or script into a page the
// owner opens on their phone while setting the device up. This is the F-03
// control from the 2026-07-25 audit.
//
// ALL FIVE characters matter, and the apostrophe is not optional padding: the
// attribute above is single-quoted, so `'` is the one that closes it. An
// earlier version of the portal escaped only `& < >` and was fixed for exactly
// that reason.
//
// TEMPLATED so the firmware keeps Arduino String while the host test drives
// std::string. Both provide length(), operator[], reserve() and += for char and
// const char*, so the code under test is byte-for-byte the code that ships —
// which is the whole point of extracting it from ota_portal.cpp, where it was
// `static` and therefore unreachable from any test.
template <class Str>
Str htmlEscape(const Str& s) {
  Str o;
  o.reserve(s.length() + 8);
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
