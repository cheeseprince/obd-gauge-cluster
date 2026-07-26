---
name: New vehicle / PID contribution
about: Share scan results or a drive log to help add support for a vehicle
title: "[vehicle] <year make model engine>"
labels: vehicle
---

<!-- Enhanced PIDs must be measured on the actual vehicle. Even a partial map or a raw
     drive log is useful. See CONTRIBUTING.md and docs/PORTING-LESSONS.md. -->

## Vehicle
- Year / make / model:
- Engine (and RPO / engine code if known):
- Architecture (e.g. GM Global B, if known):

## Adapter
- OBD adapter used (make/model — must be BLE; classic-BT and WiFi are not supported):
- Addressing that answered (11-bit `7Ex` / 29-bit `18DAxxF1` / unknown):

## What you have
<!-- Attach any of these — the more the better -->
- [ ] `obd_scan census` output (`census.json`)
- [ ] `obd_scan sweep` output (`sweep.json`)
- [ ] A drive log (`obd_scan log` CSV)
- [ ] A `correlate` report
- [ ] PIDs you've already identified (address → parameter → formula)

## Notes
<!-- Anything odd: PIDs that only answered under load, a cold-start reading, DIC values to
     compare against, etc. -->
