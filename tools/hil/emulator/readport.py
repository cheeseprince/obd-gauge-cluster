#!/usr/bin/env python3
"""Print a serial port's output. Use this instead of `cat`.

    python3 readport.py /dev/ttyACM0 [seconds]

WHY THIS EXISTS
`cat /dev/ttyACM0` returns NOTHING on this board, and the result looks exactly
like a dead device. The ESP32-S3's native USB CDC only starts emitting once DTR
is asserted, and `cat` never touches modem-control lines. Four consecutive
captures came back empty during Phase 2 bring-up before that was spotted.

pyserial raises DTR on open, so this works. The rig itself is unaffected — it
already uses pyserial — so this is purely for hand debugging.

The native port re-enumerates on every reset, which invalidates an already-open
handle: reset the board FIRST, then start this. Otherwise you capture nothing and
blame the firmware.
"""
import sys
import time

import serial


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    port = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

    with serial.Serial(port, 115200, timeout=1) as s:
        s.dtr = True    # the entire reason this script exists
        s.rts = False   # asserting RTS as well would hold the board in reset
        end = time.time() + seconds
        while time.time() < end:
            line = s.readline()
            if line:
                sys.stdout.write(line.decode("ascii", "replace"))
                sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
