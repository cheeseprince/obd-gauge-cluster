#!/usr/bin/env python3
"""CI guard — keep real VIN data out of the repository.

Allowlist only: the sole 17-char VIN-shaped tokens permitted anywhere in tracked
files are the synthetic test VINs in ALLOWED_VINS (…123456 / …000000 style, no
relation to any real vehicle). Any other VIN-shaped token fails CI — so a real
VIN pasted anywhere is caught, and this file stores NOTHING derived from a real
VIN. (An earlier version also pinned SHA-256 hashes of specific real fragments,
but SHA-256 of a 6-digit serial is brute-forced instantly, so those hashes were
themselves a small leak — the allowlist needs no such data.)

Run from anywhere in the repo:  python3 scripts/check_no_pii.py
Exit 0 = clean, 1 = a violation (prints file + the offending token).
"""
import re
import subprocess
import sys
import pathlib

# Synthetic test VINs — the ONLY VIN-shaped 17-char tokens allowed in the tree.
ALLOWED_VINS = {
    "1GT0123456789ABCD",   # GM (test_vin, framework)
    "1FT7W2BT0N2345678",   # Ford F-250 6.7 (test_vin)
    "1FT8W3BT0N2345678",   # Ford F-350 6.7 (test_vin)
    "3FT7W2BT0N2345678",   # Ford F-250 6.7, 3FT WMI (test_vin)
    "1FT7X2BT0N2345678",   # Ford non-Super-Duty negative (test_vin)
    "1FT7W2B90N2345678",   # Ford non-6.7 negative (test_vin)
    "1FT9W2BT0N2345678",   # Ford bad vin[3] negative (test_vin)
    # Ford Super Duty 6.7 model-year gate (test_vin). Same synthetic ...2345678
    # tail as the rows above; only vin[9], the model-year code, varies, because
    # the ford_sd_67 profile is gated on the 10R140 years (L..T = 2020-2026).
    "1FT7W2BT0L2345678",   # 2020 (L) — first 10R140 year, inside the gate
    "1FT8W3BT0M2345678",   # 2021 (M) — the model year actually scanned
    "1FT7W2BT0K2345678",   # 2019 (K) — 6R140, must fall to Standard+
    "1FT7W2BT0V2345678",   # 2027 (V) — unverified year, must fail closed
    "1FT9W4BT0M2345678",   # F-450 2021 — the series digit is not gated
    "WAU0123456789ABCD",   # Audi
    "WAU0123456789ABCE",   # Audi (variant)
    "WA10123456789ABCD",   # Audi (WA1 WMI-case test)
    "WBA0123456789ABCD",   # BMW
    "1FT0123456789ABCD",   # Ford
    "JHM0123456789ABCD",   # Honda (unknown-WMI test)
    "3GT0123456789ABCD",   # GM (3GT WMI-case test)
    # GM 1500 engine-discriminator tests (test_vin). Synthetic: a real GM VIN has
    # a model-year letter at position 10 and a 6-digit sequential tail, never a
    # "012345678" run. Positions that matter to the test are vin[3], vin[4] and
    # vin[7]; everything after is filler.
    "3GTUUEE80S2345678",   # GM 1500, LZ0 3.0 diesel  -> gm_sierra_lz0
    "1GTUUEE80S2345678",   # GM 1500, LZ0 3.0 diesel  -> gm_sierra_lz0 (1GT WMI)
    "3GTUUEED0S2345678",   # GM 1500, L84 5.3 gas     -> nullptr
    "3GTUUEEL0S2345678",   # GM 1500, L87 6.2 gas     -> nullptr
    "3GTUUEEK0S2345678",   # GM 1500, L3B 2.7 gas     -> nullptr
    "3GTU8EEE0S2345678",   # Sierra HD (vin[4]='8')   -> nullptr
    # BMW F10 discriminator tests (test_vin). vin[5] is the model digit.
    "WBAFR7C5012345678",   # F10 535i RWD             -> bmw_f10_535i
    "WBAFU7C5012345678",   # F10 535i xDrive          -> bmw_f10_535i
    "WBAFR1C5012345678",   # F10 528i (also 3.0 I6)   -> nullptr
    "WBAFR9C5012345678",   # F10 550i 4.4 V8          -> nullptr
    # Audi Q5 discriminator tests. vin[4] = Q5 vs SQ5, vin[7] = model line.
    "WA1ANAFY012345678",   # Q5 2.0T                  -> audi_q5
    "WA1BNAFY012345678",   # Q5 2.0T, other trim      -> audi_q5
    "WA1A4AFY012345678",   # SQ5 3.0 V6               -> nullptr
    "WA1ANAF1012345678",   # Q8                       -> nullptr
    "WA1ANAF7012345678",   # Q7                       -> nullptr
    # Jeep Wagoneer discriminator tests. vin[7] = engine.
    "1C4SJVBT0N2345678",   # Wagoneer 5.7 Hemi        -> jeep_ws
    "1C4SJVBP0N2345678",   # Wagoneer 3.0 Hurricane   -> nullptr
    "1C4SJVBJ0N2345678",   # Wagoneer 6.4 V8          -> nullptr
    "1C4SJVET0N2345678",   # Grand Wagoneer           -> nullptr
    "1C4SJXBT0N2345678",   # Wrangler                 -> nullptr
    "1C4SFVBT0N2345678",   # Ducato van               -> nullptr
    # Model-year gate tests (test_vin). vin[9] is VIN position 10, the model
    # year: N=2022, P=2023, S=2025, T=2026, V=2027, B=2011.
    "3GTUUEE80P2345678",   # GM LZ0 MY2023                -> gm_sierra_lz0
    "3GTUUEE80T2345678",   # GM LZ0 MY2026                -> gm_sierra_lz0
    "3GTUUEE80V2345678",   # GM MY2027, unverified year   -> nullptr (fails closed)
    "1GCNHEE80B2345678",   # 2011 Express van 6.6 Duramax -> nullptr
    "1C4SJVBT0P2345678",   # Wagoneer 5.7 MY2023          -> jeep_ws
    "1C4SJVBT0T2345678",   # MY2026 is Grand Wagoneer     -> nullptr
    "1J4SJVBT0N2345678",   # 1J4 is not a Wagoneer WMI    -> nullptr
    "1C40123456789ABCD",   # synthetic (test_vin)
    "1C6SJ6FT0L2345678",   # synthetic (test_vin)
    "1C6SR2FL0L2345678",   # synthetic (test_vin)
    "1C6SR4FL0L2345678",   # synthetic (test_vin)
    "1C6SR6FJ0L2345678",   # synthetic (test_vin)
    "1C6SR6FT0C2345678",   # synthetic (test_vin)
    "1C6SR6FT0L2345678",   # synthetic (test_vin)
    "1FT7A2A60N2345678",   # synthetic (test_vin)
    "1FT7A2AN0N2345678",   # synthetic (test_vin)
    "1FT7W2BT0A2345678",   # synthetic (test_vin)
    "1FT7W2BT0T2345678",   # synthetic (test_vin)
    "1FTAA2AT0N2345678",   # synthetic (test_vin)
    "1FTAA4AT0N2345678",   # synthetic (test_vin)
    "1FTAA5AT0N2345678",   # synthetic (test_vin)
    "1FTFW1ET0L2345678",   # synthetic (test_vin)
    "1FTZZ3ZT0N2345678",   # synthetic (test_vin)
    "1GC49PE70N2345678",   # synthetic (test_vin)
    "1GCUUEEK0S2345678",   # synthetic (test_vin)
    "1GT49PEY0N2345678",   # synthetic (test_vin)
    "1GT49PEY0S2345678",   # synthetic (test_vin)
    "1GTUUEED0S2345678",   # synthetic (test_vin)
    "3C6SR6FG0L2345678",   # synthetic (test_vin)
}

# A VIN token excludes I/O/Q; a hex hash never has a letter past A-F, so requiring
# one letter in G–Z (minus I/O/Q) separates real VIN tokens from SHA digests and
# other 17-char hex/base blobs. Case-INSENSITIVE: a real VIN committed in
# lowercase must still be caught (the tokens are upper()'d before the allowlist
# check below).
# \b-anchored so only a STANDALONE 17-char token matches, not a 17-char window
# inside a longer run (e.g. the base64 of the OTA public key in ota_pubkey.h).
VIN_RE = re.compile(r"\b[A-HJ-NPR-Z0-9]{17}\b", re.IGNORECASE)
VIN_LETTER = re.compile(r"[G-HJ-NPR-Z]", re.IGNORECASE)
SKIP_SUFFIX = {".png", ".jpg", ".jpeg", ".pdf", ".bin", ".sig", ".stl",
               ".ico", ".gz", ".zip", ".woff", ".woff2", ".ttf"}


def tracked_files(root: str):
    out = subprocess.check_output(["git", "-C", root, "ls-files", "-z"])
    for raw in out.split(b"\0"):
        if not raw:
            continue
        p = pathlib.Path(root) / raw.decode()
        if p.suffix.lower() in SKIP_SUFFIX:
            continue
        yield p


def main() -> int:
    root = subprocess.check_output(
        ["git", "rev-parse", "--show-toplevel"], text=True).strip()
    self_path = pathlib.Path(__file__).resolve()
    violations = []
    for p in tracked_files(root):
        if p.resolve() == self_path:      # never scan the guard itself
            continue
        try:
            text = p.read_text(errors="ignore")
        except OSError:
            continue
        for m in VIN_RE.finditer(text):
            tok = m.group(0)
            if VIN_LETTER.search(tok) and tok.upper() not in ALLOWED_VINS:
                violations.append((str(p.relative_to(root)),
                                   f"non-allowlisted VIN token {tok!r}"))

    if violations:
        print("VIN/PII guard FAILED:")
        for path, reason in sorted(set(violations)):
            print(f"  {path}: {reason}")
        print("\nOnly the synthetic test VINs in ALLOWED_VINS may appear. "
              "Never commit real VIN data (add a new synthetic VIN to the "
              "allowlist if you need another test value).")
        return 1
    print("VIN/PII guard OK — only synthetic test VINs present.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
