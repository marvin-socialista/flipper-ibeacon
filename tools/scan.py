#!/usr/bin/env python3
"""Scan for nearby iBeacons and copy one onto the Flipper.

The Flipper's firmware exposes only the transmit side of Bluetooth to apps, so
the app itself cannot listen. This script does the listening on your computer
and writes the result straight into the app's settings file, which is plain text
for exactly this reason.

    pip install bleak

    python3 tools/scan.py              # scan and list what is nearby
    python3 tools/scan.py --save       # scan, pick one, store it on the Flipper

Close the app on the Flipper before using --save: it writes its settings when
you change something, and would overwrite what this script just wrote.

macOS note: Bluetooth must be on, and the terminal needs Bluetooth permission
the first time.
"""

import argparse
import asyncio
import glob
import os
import re
import select
import sys
import termios
import time
import tty

SETTINGS_PATH = "/ext/apps_data/ibeacon/settings.conf"
APPLE_COMPANY_ID = 76  # 0x004C
PROMPT = b">: "


# --- Flipper serial CLI -----------------------------------------------------


class Flipper:
    """Minimal client for the Flipper's serial console.

    No pyserial: the Flipper shows up as USB CDC, and on macOS and Linux you can
    open that as a file as long as you put the terminal in raw mode.
    """

    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self.saved = termios.tcgetattr(self.fd)
        tty.setraw(self.fd)

    def close(self):
        termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        os.close(self.fd)

    def write(self, data, chunk=192, pause=0.01):
        # In pieces, or the CDC buffer overruns.
        for i in range(0, len(data), chunk):
            os.write(self.fd, data[i:i + chunk])
            time.sleep(pause)

    def read_until(self, token, timeout=5.0):
        buf = b""
        deadline = time.time() + timeout
        while time.time() < deadline:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    continue
                if token in buf:
                    return buf
        return buf

    def command(self, cmd, timeout=6.0):
        self.write(cmd.encode() + b"\r\n")
        return self.read_until(PROMPT, timeout=timeout)

    def read_file(self, path):
        out = self.command("storage read " + path, timeout=8).decode("utf-8", "replace")
        # The reply starts with an echo of the command and a "Size: N" line.
        m = re.search(r"Size:\s*(\d+)\s*\r?\n", out)
        if not m:
            return ""
        body = out[m.end():]
        return body[: int(m.group(1))]

    def write_file(self, path, text):
        self.command("storage mkdir " + os.path.dirname(path))
        self.command("storage remove " + path)
        self.write(("storage write " + path + "\r\n").encode())
        self.read_until(b"Ctrl+C", timeout=4.0)
        time.sleep(0.2)
        self.write(text.encode())
        time.sleep(0.3)
        self.write(b"\x03")  # Ctrl+C ends the write
        self.read_until(PROMPT, timeout=6.0)


def find_port():
    ports = glob.glob("/dev/cu.usbmodemflip_*") + glob.glob("/dev/ttyACM*")
    return ports[0] if ports else None


# --- Scanning ---------------------------------------------------------------


async def scan(seconds):
    try:
        from bleak import BleakScanner
    except ImportError:
        sys.exit("bleak is not installed. Run: pip install bleak")

    found = {}

    def seen(device, adv):
        data = adv.manufacturer_data.get(APPLE_COMPANY_ID)
        # iBeacon: 0x02 0x15, then 16 byte uuid, major, minor, measured power.
        if not data or len(data) < 23 or data[0] != 0x02 or data[1] != 0x15:
            return
        uuid = data[2:18].hex().upper()
        major = int.from_bytes(data[18:20], "big")
        minor = int.from_bytes(data[20:22], "big")
        power = int.from_bytes(data[22:23], "big", signed=True)
        found[(uuid, major, minor)] = {
            "uuid": uuid,
            "major": major,
            "minor": minor,
            "power": power,
            "rssi": adv.rssi,
        }

    scanner = BleakScanner(detection_callback=seen)
    await scanner.start()
    await asyncio.sleep(seconds)
    await scanner.stop()
    return sorted(found.values(), key=lambda b: -b["rssi"])


def pretty_uuid(hexstr):
    h = hexstr.lower()
    return "%s-%s-%s-%s-%s" % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


# --- Settings file ----------------------------------------------------------


def merge(existing, beacon, name):
    """Set the beacon as current and append it as a saved profile."""
    lines = [ln for ln in existing.splitlines() if ln.strip()]
    kept = [ln for ln in lines if not ln.startswith(("uuid=", "major=", "minor="))]

    profiles = [ln for ln in kept if ln.startswith("profile=")]
    others = [ln for ln in kept if not ln.startswith("profile=")]

    entry = "profile=%s|%s|%d|%d" % (name, beacon["uuid"], beacon["major"], beacon["minor"])
    if entry not in profiles:
        if len(profiles) >= 8:
            print("Note: 8 profiles already saved, the oldest is dropped.")
            profiles = profiles[1:]
        profiles.append(entry)

    head = [
        "uuid=" + beacon["uuid"],
        "major=%d" % beacon["major"],
        "minor=%d" % beacon["minor"],
    ]
    return "\n".join(head + others + profiles) + "\n"


# --- Main -------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--save", action="store_true", help="store the chosen beacon on the Flipper")
    ap.add_argument("--seconds", type=float, default=8.0, help="how long to scan (default 8)")
    args = ap.parse_args()

    print("Scanning for %.0f seconds..." % args.seconds)
    beacons = asyncio.run(scan(args.seconds))

    if not beacons:
        sys.exit("No iBeacons heard. Is one actually transmitting, and is Bluetooth on?")

    print("\n  #  RSSI  UUID                                  major  minor")
    for i, b in enumerate(beacons, 1):
        print("%3d  %4d  %s  %5d  %5d" % (i, b["rssi"], pretty_uuid(b["uuid"]), b["major"], b["minor"]))

    if not args.save:
        print("\nRun again with --save to copy one onto the Flipper.")
        return

    port = find_port()
    if not port:
        sys.exit("\nNo Flipper found. Plug it in, and close qFlipper if it is running.")

    choice = input("\nWhich one? [1-%d] " % len(beacons)).strip()
    if not choice.isdigit() or not 1 <= int(choice) <= len(beacons):
        sys.exit("Cancelled.")
    beacon = beacons[int(choice) - 1]

    name = input("Name it [max 16 chars]: ").strip()[:16]
    if not name:
        sys.exit("A name is required.")
    if "|" in name:
        sys.exit("The name cannot contain '|', it separates the fields in the file.")

    flipper = Flipper(port)
    try:
        flipper.read_until(PROMPT, timeout=2.0)
        flipper.write(b"\r\n")
        flipper.read_until(PROMPT, timeout=3.0)

        existing = flipper.read_file(SETTINGS_PATH)
        flipper.write_file(SETTINGS_PATH, merge(existing, beacon, name))
        check = flipper.command("storage stat " + SETTINGS_PATH)
    finally:
        flipper.close()

    print("\nSaved as '%s'." % name)
    print(check.decode("utf-8", "replace").strip().splitlines()[-2:][0])
    print("Open the app on the Flipper; it loads the file at startup.")


if __name__ == "__main__":
    main()
