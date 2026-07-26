# Security policy

This is a hobby project provided without warranty (see [`LICENSE`](LICENSE)). It reads
diagnostic data over the OBD-II port; the scanner is **read-only by construction** and
cannot transmit a write to a vehicle. You are responsible for what you connect to your own
vehicle.

## Reporting a vulnerability

Please **do not open a public issue** for a security problem. Use GitHub's private
reporting instead:

- **[Open a private security advisory](https://github.com/cheeseprince/obd-gauge-cluster/security/advisories/new)**
  — or go to the **Security** tab → **Report a vulnerability** (private vulnerability
  reporting is enabled on this repository).

That routes it privately to the maintainer. Include what the issue is, how to reproduce it,
and the impact you see. There is no formal SLA on a hobby project, but reports are
appreciated and will be looked at.

## Scope

Relevant concerns include anything that could cause the firmware or tools to **write to or
alter a vehicle** (the read-only guarantee), leak credentials or personal data, or execute
untrusted input. General bugs that don't have a security impact should go to the normal
[issue tracker](../../issues).

## BLE adapter trust model

The dash pairs with the OBD adapter headlessly — there is no human in the loop to compare
a pairing passkey, and clone ELM327 adapters ship with a fixed, publicly-known PIN. So
instead of claiming MITM-resistant pairing, bonding is **pinned to the stored adapter**:

- While **no adapter is cached** (first setup, or right after "Forget adapter" in the
  menu), the dash bonds with the first scanned device that connects and exposes a known
  ELM327 GATT profile. You initiate that window, but you do **not** choose the peer — the
  scan picks by name hint and signal strength. Treat it as trust-on-first-use: do first
  pairing somewhere you trust the RF neighborhood, and check the dash shows your adapter's
  name. A successful hijack of this window requires a purpose-built clone in range at that
  moment.
- Once an adapter is cached, pairing is auto-confirmed **only** for that stored BLE
  address; any other device's pairing attempt is refused (the connection status screen
  shows the refusal). To switch adapters, use "Forget adapter" first.

**Accepted residual risk:** an attacker with a clone present during the trust-on-first-use
window, or one who spoofs the bonded adapter's BLE address, could still impersonate the
adapter. The impact is fabricated *gauge readings* only — the firmware remains read-only
toward the vehicle by construction, and no credentials transit the OBD link.
