#include <cstdio>
#include "ble_rank.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
  // --- service-UUID filtering (UX-6) --------------------------------------
  //
  // Measured 2026-08-05 on a real RF environment: of 15 devices in range, 6
  // advertised service UUIDs and 9 advertised none. The project's own vLinker
  // advertises 0x18f0 in its advertisement, which is what makes preferring a
  // service match safe rather than a gamble.
  //
  // The rule has to be asymmetric, and that asymmetry is the whole design:
  //   Obd   -> try FIRST
  //   None  -> still try; a silent device cannot be ruled out
  //   Other -> SKIP; it positively declared a service set that is not ours
  // Skipping "None" would be the tempting simplification and would strand any
  // adapter that does not advertise its service UUID.
  {
    BleCand obd{"vLinker MS", -80, SvcHint::Obd};
    BleCand quiet{"", -40, SvcHint::None};
    BleCand other{"Living Room TV", -40, SvcHint::Other};

    check(!bleShouldSkip(obd),   "advertises our service -> never skipped");
    check(!bleShouldSkip(quiet), "advertises NO services -> still tried (fail-safe)");
    check(bleShouldSkip(other),  "advertises other services only -> skipped");

    // A service match outranks BOTH a name hint and a much stronger signal.
    BleCand cands[3] = {quiet, obd, BleCand{"OBDII dongle", -30, SvcHint::None}};
    int order[3];
    bleRankCandidates(cands, 3, order);
    check(order[0] == 1, "service match ranks above a name hint and a stronger signal");
  }

  // A name hint still beats a bare strong signal when nobody advertises a service,
  // i.e. the previous behaviour survives for adapters that advertise nothing.
  {
    BleCand cands[2] = {BleCand{"", -30, SvcHint::None}, BleCand{"vLinker", -85, SvcHint::None}};
    int order[2];
    bleRankCandidates(cands, 2, order);
    check(order[0] == 1, "name hint still wins when no service UUIDs are advertised");
  }

  // --- name-hint detection (case-insensitive substring) ---
  check(bleNameLooksLikeObd("OBDII"),        "OBDII matches");
  check(bleNameLooksLikeObd("vLinker MS"),   "vLinker matches (vlink)");
  check(bleNameLooksLikeObd("IOS-Vlink"),    "IOS-Vlink matches (vlink)");
  check(bleNameLooksLikeObd("ELM327-BLE"),   "ELM327 matches");
  check(bleNameLooksLikeObd("veepeak"),      "lowercase veepeak matches");
  check(bleNameLooksLikeObd("OBDLink LX"),   "OBDLink matches");
  check(!bleNameLooksLikeObd("iPhone"),      "iPhone does not match");
  check(!bleNameLooksLikeObd("Galaxy Buds"), "Galaxy Buds does not match");
  check(!bleNameLooksLikeObd(""),            "empty name no match");
  check(!bleNameLooksLikeObd(nullptr),       "null name no match");

  // --- an OBD-named weak device beats a strong non-OBD device ---
  {
    BleCand cands[] = {
      {"iPhone",       -40},   // 0: strong, not OBD
      {"OBDII",        -85},   // 1: weak, OBD
      {"Galaxy Watch", -50},   // 2: not OBD
    };
    int order[3];
    bleRankCandidates(cands, 3, order);
    check(order[0] == 1, "OBD-named tried first despite weaker RSSI");
    check(order[1] == 0, "then strongest non-OBD (iPhone -40)");
    check(order[2] == 2, "then next non-OBD (Watch -50)");
  }

  // --- two OBD adapters ordered by RSSI (stronger first) ---
  {
    BleCand cands[] = {
      {"OBD-weak",     -80},
      {"vLink-strong", -55},
    };
    int order[2];
    bleRankCandidates(cands, 2, order);
    check(order[0] == 1, "stronger OBD adapter first");
    check(order[1] == 0, "weaker OBD adapter second");
  }

  // --- stability: equal rank keeps input order ---
  {
    BleCand cands[] = { {"A", -60}, {"B", -60} };
    int order[2];
    bleRankCandidates(cands, 2, order);
    check(order[0] == 0 && order[1] == 1, "equal keys keep input order (stable)");
  }

  // --- empty and single-device inputs don't crash ---
  { int order[1]; bleRankCandidates(nullptr, 0, order); check(true, "n=0 no crash"); }
  { BleCand c[] = {{"OBDII", -70}}; int order[1]; bleRankCandidates(c, 1, order);
    check(order[0] == 0, "single device"); }

  // --- session-scoped reject ring -------------------------------------
  // The dash connects to a stranger, finds no ELM327 GATT profile, and
  // disconnects. Without memory it ranks that same stranger just as highly on
  // the next scan and probes it again -- which is why a laptop in the garage
  // gets a Bluetooth connection request every minute.
  bleRejectClear();
  check(!bleRejectContains("aa:bb:cc:dd:ee:ff"), "reject: empty ring skips nothing");

  bleRejectRecord("aa:bb:cc:dd:ee:ff");
  check(bleRejectContains("aa:bb:cc:dd:ee:ff"), "reject: recorded address is skipped");
  check(!bleRejectContains("11:22:33:44:55:66"), "reject: other addresses unaffected");

  // Case-insensitive: BlueZ and NimBLE disagree on hex case for the same peer.
  check(bleRejectContains("AA:BB:CC:DD:EE:FF"), "reject: match is case-insensitive");

  // Recording twice must not consume two slots.
  bleRejectRecord("aa:bb:cc:dd:ee:ff");
  bleRejectRecord("11:22:33:44:55:66");
  check(bleRejectContains("aa:bb:cc:dd:ee:ff"), "reject: duplicate keeps the original");
  check(bleRejectContains("11:22:33:44:55:66"), "reject: second address recorded");

  // Ring holds 8. Overflowing drops the OLDEST -- that device gets probed once
  // more, which is degradation rather than failure.
  bleRejectClear();
  char a[24];
  for (int i = 0; i < 8; i++) { snprintf(a, sizeof a, "00:00:00:00:00:%02d", i); bleRejectRecord(a); }
  check(bleRejectContains("00:00:00:00:00:00"), "reject: 8 entries all held");
  check(bleRejectContains("00:00:00:00:00:07"), "reject: newest held");
  bleRejectRecord("99:99:99:99:99:99");
  check(!bleRejectContains("00:00:00:00:00:00"), "reject: 9th evicts the oldest");
  check(bleRejectContains("00:00:00:00:00:07"), "reject: eviction keeps the rest");
  check(bleRejectContains("99:99:99:99:99:99"), "reject: 9th is present");

  // Null / empty must never match -- a device with no address must not be
  // silently skipped forever.
  check(!bleRejectContains(nullptr), "reject: nullptr never matches");
  check(!bleRejectContains(""), "reject: empty never matches");
  bleRejectRecord(nullptr);           // must not crash or consume a slot
  bleRejectRecord("");
  check(bleRejectContains("99:99:99:99:99:99"), "reject: null/empty record is a no-op");

  bleRejectClear();
  check(!bleRejectContains("99:99:99:99:99:99"), "reject: clear empties the ring");

  // Over-length addresses are REFUSED, not truncated. Truncation would both break
  // self-lookup and prefix-collide with an unrelated device.
  bleRejectClear();
  bleRejectRecord("aa:bb:cc:dd:ee:ff:00:11");        // 23 chars, too long
  check(!bleRejectContains("aa:bb:cc:dd:ee:ff:00:11"), "reject: over-length is not stored");
  check(!bleRejectContains("aa:bb:cc:dd:ee:ff"),       "reject: over-length cannot collide with a shorter address");
  bleRejectRecord("11:22:33:44:55:66");
  check(bleRejectContains("11:22:33:44:55:66"), "reject: over-length record did not consume a slot");

  // bleShouldSkip folds the ring in: same question, one predicate. The connect
  // path already calls it, so no new skip site is needed.
  { BleCand c{}; c.name = "vLinker MS-B"; c.rssi = -55; c.svc = SvcHint::None;
    c.addr = "de:ad:be:ef:00:01";
    check(!bleShouldSkip(c), "shouldSkip: unknown device is still tried");
    bleRejectRecord("de:ad:be:ef:00:01");
    check(bleShouldSkip(c), "shouldSkip: rejected address is skipped");
    bleRejectClear();
    check(!bleShouldSkip(c), "shouldSkip: clear restores it");
  }
  { BleCand c{}; c.name = "phone"; c.rssi = -40; c.svc = SvcHint::Other;
    c.addr = nullptr;
    check(bleShouldSkip(c), "shouldSkip: advertised-other still skipped with no addr");
  }

  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("all ble_rank tests passed\n");
  return 0;
}
