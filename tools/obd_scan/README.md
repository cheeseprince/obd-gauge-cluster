# obd_scan

A host-side OBD-II discovery scanner: point it at an unknown vehicle over a
WiFi ELM327 adapter and it works out which CAN headers answer, which PID
blocks exist, logs a drive, and statistically ranks which decoded byte
offsets correlate with known reference signals (RPM, speed, coolant, etc.) —
the same reasoning that identified GM oil pressure by hand, automated.

## Safety: this tool is read-only by construction

**It cannot write to a vehicle.** Every request this tool is capable of
sending — the OBD-II services (`catalog.ALLOWED_MODES`: `01` current data,
`03` stored DTCs, `09` vehicle info, `22` read-by-identifier) and the AT
commands (`catalog.ALLOWED_AT`) — is a read-only whitelist enforced in
`catalog.py` and checked by `elm.ElmSession` before anything reaches the
socket, *and* again by the CLI at startup (`catalog.validate_preset`) so a
malformed preset is rejected before a session even opens. There is no write
service, no reflash path, no actuator-control mode anywhere in this tool.
Any request that fails validation raises `catalog.UnsafeRequest` and the CLI
refuses to run (exit code 2) rather than transmit it. This matters because
this tool is routinely pointed at vehicles that are not yours.

## Dependencies

`numpy` and `pandas` (used by `correlate.py`'s interpretation search and
CSV/anchor handling). Everything else is standard library. Install with
whatever matches how you already manage Python packages, e.g.:

```
pip install numpy pandas
```

**Run as a module from `tools/`** (the directory containing
`obd_scan/`), not from inside `obd_scan/` itself:

```
cd tools
python3 -m obd_scan census --vehicle ford
```

## Running the tests

```
cd tools/obd_scan
python3 -m pytest tests/ -q
```

No adapter or vehicle required — every stage is exercised against a fake
scripted ELM327 over a real loopback TCP socket (`tests/fake_elm.py`).

## Adapter setup

1. Power up the OBD adapter (Vgate iCar Pro WiFi or equivalent transparent
   ELM327-over-WiFi adapter) plugged into the vehicle's OBD-II port.
2. On your laptop/phone/host, join the adapter's SoftAP WiFi network,
   normally named **`V-LINK`**.
3. The adapter's ELM327 socket defaults to **`192.168.0.10:35000`**. Every
   subcommand accepts `--host`/`--port` if your adapter differs:

   ```
   python3 -m obd_scan --host 192.168.0.10 --port 35000 census --vehicle ford
   ```

## Session flow

Four subcommands, run in this order, each consuming the previous stage's
output file:

```
python3 -m obd_scan census    --vehicle {audi,bmw,ford,gm,jeep} -o census.json
python3 -m obd_scan sweep     --vehicle {audi,bmw,ford,gm,jeep} --census census.json -o sweep.json
python3 -m obd_scan log       --sweep sweep.json -o drive.csv --hz 1.0
python3 -m obd_scan correlate drive.csv -o report.md [--pdf]
```

- **`census`** — probes every candidate 11-bit and 29-bit header with a
  handful of cheap requests to find out which modules exist and how to
  address them. Writes `census.json` (per-header alive/evidence + generic
  supported-PID list).
  `--vehicle auto` (the default) reads the VIN and picks the preset from its
  WMI; pass an explicit preset when the VIN is unreadable or you want to
  override it.
- **`sweep`** — for every header `census` found alive, walks the vehicle
  preset's PID blocks (e.g. Ford's `22F4xx`) and records every PID that
  returned a positive reply. Reads `census.json`, writes `sweep.json`
  (the hit list). **This is the slow stage — budget roughly 100 seconds per
  header for a 5-block preset** (ATAT2 adaptive timing gets a NO DATA back
  in ~60–100 ms per probe, but a block is up to 256 probes). Do not kill it
  thinking it hung; the `progress` line updates every probe.
- **`log`** — round-robins the sweep's hit list plus the generic anchor PIDs
  (RPM, speed, load, coolant, MAF, baro, ambient) during an actual drive,
  writing a wide CSV. Reads `sweep.json`, writes `drive.csv`. Ctrl-C stops
  cleanly; everything already flushed to disk stays there. **`--hz` is an
  upper bound, not a guarantee** — each cycle round-trips every hit PID plus
  all 7 anchors sequentially over one TCP link (no batching), so e.g. 30
  hits + 7 anchors at ~80 ms per round trip is already ≈0.4 Hz actual;
  asking for `--hz 1.0` will not make it faster than the link allows.
- **`correlate`** — offline analysis (no adapter needed). Enumerates every
  plausible byte-offset interpretation (u8/u16/s16 at each offset) of every
  hit column, correlates each against every anchor, and ranks by best `|r|`.
  Reads a drive CSV, writes a Markdown (optionally PDF, via `--pdf`) report
  ranking every candidate PID with its best interpretation, correlated
  anchor, and a verdict (`correlated` / `weak` / `no-signal` /
  `too-few-samples` / `constant` / `sentinel` / `mirror-tautology` — see the report's own "How to
  read this" section for what each means and why `r` alone is not proof).
  `--workers` controls parallelism (default `min(os.cpu_count() or 1, 4)`).

## In-cab runbook

1. **Connect and run `census` first.** Read the `alive:` line before doing
   anything else — this tells you which headers actually answered on *this*
   adapter, on *this* vehicle, today.
2. **If no 11-bit header answers, do not conclude the vehicle has no
   OBD-II modules — try 29-bit before drawing any conclusion.** Addressing
   is adapter-dependent, not just vehicle-dependent: the same truck has been
   observed answering 11-bit on one adapter and *only* 29-bit on another.
   `census` already probes both address spaces in one pass, so just read the
   full `headers` table, not only the summary line, before deciding a truck
   is a dead end.
3. **Run `sweep`** against whichever headers came back alive. This is the
   slow step (hundreds of probes per block) — let it finish; the header is
   set once per block, not per probe, specifically to keep this fast enough
   to do in a parking lot.
4. **Get one cold-start reading per vehicle.** Start `log` the moment the
   engine is cranked, before it warms up, and let it run a minute or two at
   idle. A cold-vs-hot comparison at the *same RPM* is the strongest
   identity confirmation this tool can produce — it is what finally nailed
   down oil pressure on the GM truck, where correlation alone was
   ambiguous. Skipping the cold start throws away that confirmation
   permanently for this vehicle; there's no redoing it once the engine is
   warm.
5. **Log a drive with varied load** — idle, cruise, hills or throttle
   transients, some braking. Anchors like RPM/speed/load only vary enough to
   correlate against if the drive actually varies; a flat, idle-only log
   makes every real sensor read as `no-signal` or near-constant.
6. **Run `correlate`** back at a desk (no adapter required) to get the
   ranked report.

## When the adapter can't be reached

A link that never comes up is reported as a diagnosis, not a Python
traceback. `ElmSession.connect` and `ElmSession.init` raise
`AdapterUnreachable`, the CLI prints it verbatim under an
`ADAPTER NOT REACHED` banner and **exits 3** (distinct from `2`, which is a
read-only safety refusal — see `catalog.UnsafeRequest`).

The four cases are distinguished because each has a different fix, and
`elm.diagnose_connect_error` names the one that applies:

| Symptom | Almost always means |
| :--- | :--- |
| **timed out** after 5 s | The laptop is not joined to the adapter's SoftAP. macOS will silently roam back to a known network because `V-LINK` has no internet — re-check the WiFi menu immediately before re-running. |
| **connection refused** | Right host, wrong port — or the adapter is still booting. |
| **could not be resolved** | `--host` was given a name; it takes an IP. |
| **never answered ATZ** | TCP is open but nothing is speaking ELM327: a wedged adapter, or another client (phone in Car Scanner / Torque) holding the adapter's single connection slot. |

That last one is the case worth understanding, because it is the only one
that does not look like a failure. Before this check existed, a silent peer
let `init()` return an empty string and the census ran to completion with
every header marked `evidence="error"` — a link fault presented as a finding
about the vehicle. It now fails at the first unanswered command instead.

## What `aborted` means

`census`, `sweep`, and `log` can each be cut short mid-session by a dropped
adapter link (BLE/WiFi hiccup, adapter power loss, etc.). When that happens,
the stage does **not** pretend nothing happened: its JSON result carries
`"aborted": true` and an `"error"` string describing the failure, and the
CLI prints an unmistakable banner:

```
*** SCAN INCOMPLETE -- link failed: <error> ***
*** Results below only cover what was reached before the drop. ***
*** Reconnect the adapter and re-run this stage. ***
```

This distinction matters because the two lists behave differently on
truncation, and neither one on its own tells you whether the scan actually
finished:

- `census`'s `headers` list contains only the headers **actually reached**
  before the drop — headers never probed are simply absent from the list.
- `sweep`'s `headers_targeted`/`blocks_targeted` lists are the full
  **targeted** set regardless of how far the sweep actually got.

So an `alive: (none)` or a short hit count on an aborted run does **not**
mean the vehicle has nothing to offer — it means the link died before the
tool could find out. Everything collected before the drop is genuine and is
kept in the output file; nothing after it is inferred. If you see the
`SCAN INCOMPLETE` banner, reconnect the adapter and re-run that stage rather
than trusting the partial result as a final answer.

An `HeaderResult.evidence` of `"error"` (shown in the per-header table) means
the same thing at header granularity: the transport failed while that
specific header was being probed, so nothing was determined about it — it is
neither `"positive"` (alive), `"negative"` (alive, rejected the PID),
`"silent"` (a clean link that got a clean non-answer), nor confirmed absent.

**`evidence="error"` does not require `aborted`.** A wedged link (the
adapter accepts the connection, reads, and simply never replies) never
raises an `OSError` — `sendall()` succeeds against the kernel buffer, only
the read side goes quiet. Two consecutive reads with no ELM `>` prompt seen
are attributed to the link, not the vehicle, and always classify as
`evidence="error"`, even on a run where `aborted` stays `False`. Because of
this, `cmd_census` prints an explicit banner whenever any header comes back
`"error"`:

```
*** N header(s) UNDETERMINED (transport fault) — not a finding. ***
*** No header was successfully determined. Check the adapter link and re-run
    before concluding anything about this vehicle. ***    (only if alive: (none))
```

`sweep` and `log` count the same fault (`"errors"` / `"error_polls"` in
their JSON output) and print a similar warning when a meaningful fraction of
probes/polls hit it — a mostly-erroring run is not a negative finding either,
even when no single probe ever raised `OSError`.

**Truncation also propagates forward.** If `census.json` or `sweep.json` was
itself written with `"aborted": true`, the next stage that reads it (`sweep`
reading `census.json`, `log` reading `sweep.json`) prints an
`*** INPUT INCOMPLETE ***` warning before doing anything else — running a
later stage on a truncated input silently inherits that gap otherwise.
