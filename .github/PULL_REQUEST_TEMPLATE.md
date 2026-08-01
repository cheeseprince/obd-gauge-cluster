<!-- Thanks for contributing. Seven checks must pass before this can merge: host tests on
     Linux and macOS, the device build, ruff, the image/PDF metadata scan, the PII guard,
     and the gitleaks secret scan. CONTRIBUTING.md lists what each one enforces. -->

## What this changes


## Checklist
- [ ] Host tests pass locally (`cd test && make`; and `cd tools/obd_scan && python3 -m pytest tests -q` if the scanner changed)
- [ ] Any image I added has **no metadata** (`exiftool -all= <file>` or re-saved without it)
- [ ] Docs updated if behavior changed
- [ ] For a new vehicle: the PIDs were measured on the actual vehicle (see CONTRIBUTING.md)
- [ ] **If this touches the Arduino core / IDF version, NimBLE, the BLE source, WiFi or TLS:**
      I linked to an adapter on real hardware and saw live gauge values. CI has no radio, so
      it cannot check this — see [docs/OTA.md](../docs/OTA.md#before-you-tag-link-to-an-adapter-on-real-hardware)
