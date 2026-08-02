#!/usr/bin/env python3
"""Convert ui_snapshot PPMs into the committed docs/images/*.png, or verify them.

    ./snapshot && python3 render_docs_images.py            # refresh the PNGs
    ./snapshot && python3 render_docs_images.py --check     # verify, exit 1 on drift

`--check` is what CI runs: it proves the committed screenshots still match what
the current firmware actually draws. Without it, a label or layout change lands
with stale images and the README quietly describes an older build.

Comparison is on DECODED PIXELS, never file bytes: different Pillow versions
emit different PNG compression for identical images, so a byte diff would fail
for reasons that have nothing to do with the UI.

Rendering must be deterministic for this to work — see the pinned FW_DATE in the
Makefile, without which the splash screen carries the build date and every run
differs.
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("Pillow is required: python3 -m pip install pillow")

HERE = os.path.dirname(os.path.abspath(__file__))
IMGS = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "images"))

# Committed PNG <- snapshot PPM, one for one.
DIRECT = {
    "page0_day.png":    "page0_day.ppm",
    "page1_day.png":    "page1_day.ppm",
    "page2_day.png":    "page2_day.ppm",
    "page3_day.png":    "page3_day.ppm",
    "page4_day.png":    "page4_day.ppm",
    "page5_day.png":    "page5_day.ppm",
    "page6_day.png":    "page6_day.ppm",
    "page0_night.png":  "page0_night.ppm",
    "page0_metric.png": "page0_metric.ppm",
    "focus_day.png":    "focus_day.ppm",
    "warning_tile.png": "warning_tile.ppm",
    "error_tile.png":   "error_tile.ppm",
    "alarm_zones.png":  "alarm_zones.ppm",
    "splash_day.png":   "splash_day.ppm",
    "settings.png":     "menu_day.ppm",
}

# statusbar_states.png is a composite: five 24px status bands, each with a
# hand-authored caption strip beneath it describing that state. Only the bands
# come from the renderer; the captions are artwork and have no PPM to compare
# against, so they are preserved on write and ignored on check.
COMPOSITE = "statusbar_states.png"
COMPOSITE_SRC = [
    "statusbar_log_on.ppm",    # Linked + logging
    "statusbar_bt_gray.ppm",   # Not linked
    "statusbar_log_err.ppm",   # SD write error
    "statusbar_no_sd.ppm",     # No SD card
    "statusbar_log_off.ppm",   # Logging off
]
BAND_Y, BAND_H, PITCH = 2, 24, 45


def load(path):
    return Image.open(path).convert("RGB")


def differs(a, b):
    """True if two same-mode images differ in any pixel."""
    return a.size != b.size or ImageChops.difference(a, b).getbbox() is not None


def composite(base):
    """Committed image with all five status bands replaced by freshly rendered ones."""
    out = base.copy()
    for i, ppm in enumerate(COMPOSITE_SRC):
        band = load(os.path.join(HERE, ppm)).crop((0, 0, base.width, BAND_H))
        out.paste(band, (0, BAND_Y + PITCH * i))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="verify only; write nothing and exit 1 if any render drifted")
    args = ap.parse_args()

    missing = [p for p in list(DIRECT.values()) + COMPOSITE_SRC
               if not os.path.exists(os.path.join(HERE, p))]
    if missing:
        print("Missing snapshots — run ./snapshot first:", file=sys.stderr)
        for m in sorted(missing):
            print(f"  {m}", file=sys.stderr)
        return 2

    stale, wrote, ok = [], [], []

    for png, ppm in DIRECT.items():
        new = load(os.path.join(HERE, ppm))
        dst = os.path.join(IMGS, png)
        if os.path.exists(dst) and not differs(load(dst), new):
            ok.append(png)
            continue
        if args.check:
            stale.append(png)
        else:
            new.save(dst)
            wrote.append(png)

    # Composite: compare/refresh the band rows only, never the caption artwork.
    dst = os.path.join(IMGS, COMPOSITE)
    base = load(dst)
    new = composite(base)
    if not differs(base, new):
        ok.append(COMPOSITE)
    elif args.check:
        stale.append(COMPOSITE)
    else:
        new.save(dst)
        wrote.append(COMPOSITE)

    if args.check:
        if stale:
            print(f"{len(stale)} committed render(s) no longer match the UI code:\n",
                  file=sys.stderr)
            for f in sorted(stale):
                print(f"  docs/images/{f}", file=sys.stderr)
            print("\nRegenerate them with:\n"
                  "  cd tools/ui_snapshot && make && ./snapshot && "
                  "python3 render_docs_images.py", file=sys.stderr)
            return 1
        print(f"All {len(ok)} committed renders match the current UI code.")
        return 0

    for f in sorted(wrote):
        print(f"updated  docs/images/{f}")
    print(f"\n{len(wrote)} updated, {len(ok)} already current.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
