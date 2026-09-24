# flipper-ibeacon

**Turn a Flipper Zero into a configurable iBeacon.**

A native app, not a script. It broadcasts a standard iBeacon advertisement and lets
you change the **UUID, major, minor and advertising interval on the device itself**.
Settings survive a reboot. It shows up under **Apps → Bluetooth → iBeacon Emulator**.

> Why? If you're building anything that reacts to iBeacons, the annoying part isn't
> the code, it's the hardware. You order a beacon, wait a week, and only then find
> out your region monitoring never fires. This gives you a beacon you already own,
> whose UUID you can change in ten seconds, and that runs on a battery so you can
> **walk away with it**. That last part matters more than it sounds: region *exit* is
> the half everyone gets wrong, and you cannot test it with a beacon bolted to your
> desk.

<a href="https://buymeacoffee.com/socialista"><img src="https://img.shields.io/badge/Buy%20me%20a%20coffee-%E2%98%95-yellow" alt="Buy me a coffee"></a>

---

## Install

**Download** `ibeacon.fap` from [Releases](../../releases) and copy it to
`/ext/apps/Bluetooth/` on the SD card, or build it yourself (below).

The app is built for **Momentum firmware**. A `.fap` carries the API version it was
built against and the firmware refuses it on a mismatch, so see
[Building](#building) if you run stock or a different fork.

---

## Using it

| Menu item | |
|---|---|
| **Start / Stop broadcasting** | The first row shows the current state, so you can see at a glance whether it's running |
| **Set UUID** | Hex byte editor, exactly 16 bytes. Much less painful than typing dashes on a keyboard |
| **Set major / minor** | 0 to 65535 |
| **Set interval** | 20 to 5000 ms. Apple suggests 100 ms for iBeacon |
| **Status** | What's configured and whether it's on air |

Two behaviours worth knowing, because they're deliberate:

**Leaving the app does not stop the broadcast.** The extra beacon keeps running
alongside the Bluetooth stack. That's the point: you want to exit, pocket the Flipper
and walk out of range. Stop it from the menu when you're done.

**Changing a value while it's broadcasting restarts it immediately** with the new
values. Otherwise the air would carry the old frame while the menu claims something
else.

Settings live in `/ext/apps_data/ibeacon/settings.bin`.

---

## What it actually sends

30 bytes, the standard iBeacon layout:

```
02 01 06                                          flags
1A FF 4C 00                                       manufacturer data, Apple (0x004C, little endian)
02 15                                             iBeacon subtype + length
XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX XX   proximity UUID
MM MM                                             major (big endian)
mm mm                                             minor (big endian)
C5                                                measured power, -59 dBm
```

Transmission uses the firmware's `furi_hal_bt_extra_beacon_*` API, which advertises
**alongside** the normal Bluetooth stack rather than replacing it, so you keep your
regular Flipper connection while it runs.

Verified over the air with a BLE scanner: UUID, major, minor and measured power all
decode correctly, and macOS reads it as a valid iBeacon.

---

## Building

The only tricky part is the SDK. A `.fap` records the firmware API version it was
compiled against, and the firmware refuses to launch it if that doesn't match.

```bash
python3 -m venv .venv && .venv/bin/pip install ufbt
export UFBT_HOME="$PWD/.ufbt"

# Momentum SDK. Replace the URL with the build your device runs, see below.
.venv/bin/ufbt update --hw-target f7 \
  --url https://up.momentum-fw.dev/builds/firmware/dev/flipper-z-f7-sdk-mntm-dev-8ed809fb.zip

.venv/bin/ufbt          # build, output lands in dist/
.venv/bin/ufbt launch   # build, upload to /ext/apps/Bluetooth/ and run it
```

Watch for this line in the build output:

```
APPCHK  ...  Target: 7, API: 87.1
```

That API number has to match your firmware. Check yours with `device_info` over the
Flipper's serial CLI, or in **Settings → About**.

**Finding the right SDK.** Momentum publishes every build at
[`up.momentum-fw.dev/firmware/directory.json`](https://up.momentum-fw.dev/firmware/directory.json).
Look up the `sdk_zip` url for your channel, or substitute your own
`firmware_commit` into the dev url above. For **stock firmware**, drop the `--url`
and let `ufbt` fetch the official SDK; the app uses no Momentum-specific APIs, so it
should build, though I haven't tested it there.

---

## Notes for the iOS side

Things that cost me time and aren't obvious:

- **iOS cannot discover unknown iBeacons.** Apple filters its own manufacturer data
  out of what CoreBluetooth hands you, and CoreLocation only ranges UUIDs you name
  yourself. A generic "scan for beacons nearby" feature is not possible; you can only
  range a list of UUIDs you already know.
- **Region monitoring needs "Always" location permission.** Without it nothing
  happens and there is no error, which is a miserable way to spend an afternoon.
- **iOS relaunches a terminated app on a region event, even after a force quit.**
  Measured on iOS 26.6: ~5 seconds for an enter, ~15 seconds for an exit, with the app
  swiped away in the app switcher.
- **The advertising interval barely matters for enter/exit latency.** iOS scans on its
  own schedule and that dominates. A slower interval does raise the risk of a spurious
  exit, though, since iOS decides you've left after not hearing the beacon for a while.

---

## License

MIT, see [LICENSE](LICENSE).

If this saved you a week of waiting for hardware, you can
[**buy me a coffee ☕**](https://buymeacoffee.com/socialista).
