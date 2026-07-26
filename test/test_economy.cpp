#include <cstdio>
#include <cmath>
#include "economy.h"
static int failures=0;
static void check(bool c,const char*m){if(!c){printf("FAIL: %s\n",m);failures++;}}
int main(){
  Economy e;
  e.reset();
  check(e.instantMpg()==0.0f && e.avgMpg()==0.0f && !e.valid(), "reset: zero, invalid");

  e.update(4.0f, 40.0f, 0);              // seed (no integration on first call)
  check(e.instantMpg()==10.0f, "instant 40/4 = 10");
  check(!e.valid(), "no avg until time passes");

  e.update(4.0f, 40.0f, 1000);          // +1s cruise
  check(fabsf(e.avgMpg()-10.0f) < 0.1f, "avg cruise = 10");
  check(e.valid(), "valid after a burn");

  e.update(1.0f, 0.0f, 2000);           // +1s idle (burning fuel, not moving)
  check(e.instantMpg()==0.0f, "instant 0 when stopped");
  check(e.avgMpg() < 10.0f, "avg drops with idle burn");

  float beforeGap = e.avgMpg();
  e.update(4.0f, 40.0f, 999999999);     // huge gap is ignored (reconnect)
  check(fabsf(e.avgMpg() - beforeGap) < 0.001f, "huge dt skipped: avg unchanged");

  Economy f; f.reset();
  f.update(0.01f, 0.5f, 0); f.update(0.01f, 0.5f, 1000);
  check(f.instantMpg()==0.0f, "instant guard at tiny fuel/speed");

  // --- Refinements batch: 99-cap + avg gal/100mi + L/100km ---
  // Instant cap: very low fuel + high speed would compute ~1980 mpg -> capped to 99.
  { Economy c; c.reset();
    c.update(0.05f, 99.0f, 0);          // seed
    c.update(0.05f, 99.0f, 1000);
    check(c.instantMpg() == 99.0f, "instant mpg capped at 99");
  }
  // Sane cruise: 30 mpg -> 3.333 gal/100mi, 7.84 L/100km.
  { Economy d; d.reset();
    d.update(2.0f, 60.0f, 0);           // 30 mpg instant
    d.update(2.0f, 60.0f, 1000);        // 1s cruise
    check(fabsf(d.avgMpg() - 30.0f) < 0.2f, "avg 30 mpg cruise");
    check(fabsf(d.avgGalPer100mi() - 3.333f) < 0.05f, "gal/100mi ~3.33 at 30mpg");
    check(fabsf(d.avgLPer100km() - 7.84f) < 0.1f, "L/100km ~7.84 at 30mpg");
  }
  // Idle blowup: lots of fuel, barely moving -> gal/100mi and L/100km cap at 99.
  { Economy g; g.reset();
    g.update(20.0f, 1.0f, 0);           // seed
    g.update(20.0f, 1.0f, 1000);        // 1s: ~0.05 mpg
    check(g.avgGalPer100mi() == 99.0f, "gal/100mi capped at 99 (idle)");
    check(g.avgLPer100km() == 99.0f, "L/100km capped at 99 (idle)");
  }

  printf(failures?"\n%d FAILED\n":"\nALL PASS\n",failures); return failures?1:0;
}
