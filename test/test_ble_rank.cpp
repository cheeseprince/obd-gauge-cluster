#include <cstdio>
#include "ble_rank.h"

static int failures = 0;
static void check(bool c, const char* m){ if(!c){ printf("FAIL: %s\n", m); failures++; } }

int main() {
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

  if (failures) { printf("%d FAILED\n", failures); return 1; }
  printf("all ble_rank tests passed\n");
  return 0;
}
