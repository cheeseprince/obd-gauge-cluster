#!/usr/bin/env python3
"""
check_expect.py — verify a sweep rediscovered a reference vehicle's known PIDs.

This is the acceptance gate. The Sierra is the only vehicle where every answer
is already known, so it is the only place a failure of this tool is
*recognisable*. On an unmapped truck a broken scanner and a quiet ECU look
identical — you would come home with an empty hit list and no way to tell which
one you had.

So: run the census and sweep on the Sierra, run this, and only take the tool to
somebody else's vehicle once it says GATE PASSED.

Usage:
    python3 -m obd_scan.check_expect sweep.json tests/sierra_expect.json

Exit code 0 = passed, 1 = failed, 2 = usage error.
"""
import json
import sys


def main(argv=None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    if len(argv) != 2:
        print(__doc__)
        return 2

    try:
        with open(argv[0]) as fh:
            sweep = json.load(fh)
        with open(argv[1]) as fh:
            expect = json.load(fh)
    except (OSError, json.JSONDecodeError) as e:
        print(f"could not read inputs: {e}")
        return 2

    found = {h["request"] for h in sweep.get("hits", [])}
    missing = [e for e in expect["must_find"] if e["request"] not in found]
    enough = len(found) >= expect["min_hits"]

    print(f"vehicle: {expect['vehicle']}")
    print(f"hits:    {len(found)} (minimum {expect['min_hits']})\n")
    for e in expect["must_find"]:
        mark = "ok  " if e["request"] in found else "MISS"
        print(f"  [{mark}] {e['request']}  {e['why']}")

    # A truncated sweep is not a failed gate — it is an invalid one. Say so
    # rather than reporting a pass or a fail on incomplete evidence.
    if sweep.get("aborted"):
        print(f"\nSWEEP WAS TRUNCATED: {sweep.get('error')}")
        print("GATE INVALID — the sweep did not finish. Fix the link and re-run;"
              " do not read anything into the results above.")
        return 1

    if missing:
        print(f"\n{len(missing)} known PID(s) were NOT rediscovered.")
    if not enough:
        print(f"\nOnly {len(found)} hits — expected at least {expect['min_hits']}.")

    ok = not missing and enough
    print("\nGATE PASSED — the tool found what we already know is there."
          if ok else
          "\nGATE FAILED — do not take this to a vehicle nobody has mapped.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
