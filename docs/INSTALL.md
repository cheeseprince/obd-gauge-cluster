# First-time setup

Full walkthrough for flashing the dash, provisioning it over WiFi, and getting live gauges.
The [README](../README.md) has the five-step short version — this page has the detail and
the troubleshooting.

The first install is over USB from a computer; everything after that is done from your phone
and the knob.

## 1. Prerequisites

| Tool | Notes |
| :--- | :--- |
| [PlatformIO Core](https://platformio.org/install/cli) | Builds and flashes the firmware. |
| Python 3.12 | PlatformIO's runtime. |
| git | To clone this repo. |

Verified on **macOS** and **Linux**. **Windows is untested** — PlatformIO itself supports it,
but nobody has flashed a dash from Windows with this repo yet, so treat it as unverified.

**Linux only:** your user needs to be in the `dialout` group to open the serial port without
sudo:

```
sudo usermod -aG dialout $USER
```

Log out and back in (or reboot) for the group change to take effect.

## 2. Flash the firmware

Connect the board to your computer with a **data** USB-C cable — many cables sold as
"charging cables" carry power only and won't enumerate a serial device. From a clone of this
repo, pick the build for your board:

```
pio run -e crowpanel_obd -t upload      # BLE dash (default) — CrowPanel Advance 3.5"

```

`crowpanel_obd` is the default and what most people want: it talks BLE to the adapter.
which adapters need which build.

Normal flashing uses esptool's automatic reset — there are no buttons to hold.

### If PlatformIO grabs the wrong port

Find the board's serial port and pin it explicitly with `--upload-port`:

```
ls /dev/cu.usbmodem*                    # macOS
pio run -e crowpanel_obd -t upload --upload-port /dev/cu.usbmodemXXXX
```

On Linux, the port is typically `/dev/ttyACM*` or `/dev/ttyUSB*`.

**Never glob the port** (e.g. `--upload-port /dev/cu.usbmodem*` passed through unexpanded, or
scripting around a wildcard match). macOS can leave a **permanent ghost `usbmodem` device
node** behind after a board is unplugged, so a glob can silently resolve to a stale port that
no longer has a board on it instead of the one you just connected. List the port, read the
exact name, and pass that.

## 3. Power it in the truck

Plug it into a switched USB port. The dash boots to a connecting screen.

## 4. Provision over WiFi

From your phone: long-press the knob to open **WiFi setup**. (That same long-press is the
settings menu for everything else — units, brightness, night mode, date/time, adapter,
logging.)

The dash raises a per-device WiFi network named **`OBD-XXXX`**, where `XXXX` is 4 hex digits
derived from the chip's MAC — unique per unit. The password is random per device, generated
the first time setup runs, and shown on the dash screen for the duration of the portal — it
isn't printed anywhere or knowable in advance.

Join that network. The portal is a **captive portal** on purpose, so iOS and macOS
auto-open the setup page as soon as you join. If it doesn't open automatically, browse to
`http://192.168.4.1`.

On the setup page:

- **Add your home/hotspot WiFi** — needed later for over-the-air updates.
- **Set your location** — see below.

The portal stays open for **5 minutes** from when it starts; if nothing is saved before then
it closes and the dash reboots on its own. There's no penalty for that — long-press the knob
again to reopen it.

### Location

Latitude, longitude, and UTC offset, entered as plain numbers:

- **Latitude:** -90 to 90.
- **Longitude:** -180 to 180. **West is negative** — e.g. a point in the continental US reads
  negative, a point in most of Europe reads positive. (Example only, not a real saved
  location: `40.71, -74.01` for a point near New York.)
- **UTC offset:** -12 to 14.

All three fields are required together, and each must actually be a number — the form
rejects non-numeric input rather than silently saving zeros. **There is no default
location**, so automatic day/night switching stays off until you set this. US daylight
saving is applied automatically on top of the UTC offset you enter — you don't need to
adjust it twice a year.

Saving location shows a confirmation page with two options: a **Done — reboot display**
button that finishes setup and reboots immediately, and a **Back to setup** link if you want
to add more WiFi networks or change anything else before finishing. Either the WiFi page or
the location-confirmation page's Done button ends the portal the same way.

## 5. Connect the OBD adapter

**If you bought the Vgate vLinker MS, switch it to BT+BLE first.** It ships in a
Classic/MFi-only mode and does not advertise over BLE until you change it once, using
Vgate's updater app on a phone. A stock adapter never appears in the dash's scan — which
looks exactly like a broken dash.

Plug a BLE ELM327 into the OBD-II port. The default (`crowpanel_obd`) build scans and
auto-connects to most BLE ELM327 adapters — no pairing step. It is **BLE-only**;
classic-Bluetooth (PIN-pairing) and WiFi adapters are not supported, because the ESP32-S3
has no classic-BT radio. See [`ADAPTERS.md`](ADAPTERS.md) for the full compatibility matrix,
supported GATT profiles, and known-unsupported adapters — this page won't duplicate it.

To switch adapters later, use **Forget adapter** in the settings menu.

On first connect, the firmware reads the vehicle's VIN to auto-select a gauge profile for
your vehicle — so don't be surprised if the dash reboots itself the first time it links to
the adapter in a new vehicle. See [`VEHICLES.md`](VEHICLES.md) for how that selection works
and what each profile supports.

## 6. Set the clock (optional)

**Set date/time** in the settings menu. It backs up to the coin cell, so it's a one-time
step — the dash keeps time across power cycles after that.

## Done

Gauges appear once the adapter links. From here, updates are over-the-air — see the
[README](../README.md#updates-ota) — no cable needed again.

---

## Troubleshooting

**Port not found / upload fails to connect.**
- Confirm the cable is a **data** cable, not charge-only — swap it first; this is the most
  common cause.
- Re-run `ls /dev/cu.usbmodem*` (macOS) or check `/dev/ttyACM*` / `/dev/ttyUSB*` (Linux)
  after re-plugging, and pass the exact port with `--upload-port`. Don't reuse a port name
  from a previous session without re-listing it — see the ghost-device note above.
- Linux: confirm your user is actually in `dialout` (`groups $USER`) and that you've logged
  out/in since adding it.

**Portal won't open / doesn't auto-launch.**
- Confirm your phone actually joined the `OBD-XXXX` network (not just saved it) and that the
  password on screen matches what you typed — it's generated once per device and then persisted, not a fixed fleet-wide value.
- If the captive-portal popup doesn't appear, open a browser and go to `http://192.168.4.1`
  directly.
- Remember the portal times out after 5 minutes; if it closed, reopen it with another
  long-press of the knob.

**Adapter won't link.**
- Check the adapter is BLE, not classic-Bluetooth or WiFi — see
  [`ADAPTERS.md`](ADAPTERS.md) for which adapters are validated, should-work, or
  known-unsupported.
- Try **Forget adapter** in the settings menu and let it rescan, in case a stale saved
  address is being retried.
- Confirm the adapter is actually powered — plugged into the OBD-II port with the ignition or
  accessory power on, depending on the adapter.

**Gauges stay blank.**
- Gauges only populate once the adapter has linked and the vehicle has answered PIDs with the
  ignition/accessory power on — a linked-but-idle adapter (no key power) will still show
  blank tiles.
- If the vehicle isn't one of the profiled makes, it falls back to the Generic profile — see
  [`VEHICLES.md`](VEHICLES.md) for what that profile does and doesn't support.
