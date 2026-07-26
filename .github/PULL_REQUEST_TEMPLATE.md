<!-- Thanks for contributing. The three required checks (metadata, host tests, device
     build) must pass before this can merge. -->

## What this changes


## Checklist
- [ ] Host tests pass locally (`cd test && make`; and `cd tools/obd_scan && python3 -m pytest tests -q` if the scanner changed)
- [ ] Any image I added has **no metadata** (`exiftool -all= <file>` or re-saved without it)
- [ ] Docs updated if behavior changed
- [ ] For a new vehicle: the PIDs were measured on the actual vehicle (see CONTRIBUTING.md)
