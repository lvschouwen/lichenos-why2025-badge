#!/usr/bin/env python3
"""Long-text sweep (#133): walk the badge's screens over Wi-Fi and run "fw ui overflow" on each.

Usage: tools/ui_overflow_sweep.py <badge-ip> [--only NAME] [--list]
  Exit 0: every screen clean. 1: at least one label overflows. 2: a screen could not be driven or read.
  Needs the OTA token like tools/badge.sh (see its header). Talks to the badge only through "badge.sh fw <ip> ...".

What it sends: "fw keys send <seq>" (a fresh walk from home for every screen) and "fw ui overflow". Nothing else, and
never "text:" tokens or anything that transmits: sequences containing "text:" are refused, and the chat/compose screens
are only opened, never typed into or sent from. Keep it that way when editing the table.

Fixtures (set up by hand first; bench rules: on-air targets are ONLY #badgetest and the Heltec test node, never real
contacts; this script sends nothing on air):
  - a channel with a 31-character name (a #badgetest-style channel, e.g. "#badgetest-0123456789-abcdefghij")
  - the Heltec test node renamed to a 32-character name, heard so it is in the contact list (and a repeater entry)
  - a file with a 127-character name on the SD card (and one long folder name) in the root Files shows first
  - a saved Wi-Fi network with a 32-character SSID in range or in the saved list
  - a few notifications in the centre with long text (a long channel-name message from #badgetest)
Without the fixtures the sweep still runs but only proves short data fits.

UNVERIFIED: every key sequence below was written without the badge (the build agent could not run it). Fix the table on
the bench: for each screen run the sequence by hand ("tools/badge.sh fw <ip> keys send ..." then "fw ui") and correct it.
Sequences are tokens of firmware/components/lichen_shell/ui_keys.h (up down left right enter esc tab bksp wait).
"""
import argparse
import os
import re
import subprocess
import sys

HOME = "esc esc esc esc esc"  # backs out of any depth; Esc on home does nothing (UNVERIFIED)

# (name, key sequence from home). All UNVERIFIED: tile order and menu rows are guesses. Opening only: no sequence may
# press Enter on a send/transmit/radio action. Region & radio, TX power and Custom radio are deliberately not visited.
SCREENS = [
    ("home", ""),
    ("settings", "right enter"),
    ("settings/display", "right enter down enter"),
    ("settings/sound", "right enter down down enter"),
    ("settings/wifi list", "right enter down down down enter"),
    ("settings/notifications", "right enter down down down down enter"),
    ("settings/sd card", "right enter down down down down down enter"),
    ("settings/updates", "right enter down down down down down down enter"),
    ("files", "right right enter"),
    ("files/first folder", "right right enter down enter"),
    ("release notes", "right right right enter"),
    ("notify centre", "right right right right enter"),
    ("mesh app", "down enter"),
    ("mesh/contacts", "down enter enter"),
    ("mesh/contact detail", "down enter enter enter"),
    ("mesh/repeaters", "down enter down enter"),
    ("mesh/channels", "down enter down down enter"),
    ("mesh/chat (#badgetest)", "down enter down down enter enter"),
]

BADGE_SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "badge.sh")
HIT = re.compile(r"^\s*(-?\d+),(-?\d+) (\d+)/(-?\d+) px ")


def fw(ip, line):
    out = subprocess.run([BADGE_SH, "fw", ip, line], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        raise RuntimeError(f"fw {line!r} failed: {out.stderr.strip() or out.stdout.strip()}")
    return out.stdout.splitlines()


def check_screen(ip, name, seq):
    if "text:" in seq:
        raise RuntimeError(f"{name}: text: tokens are not allowed in the sweep")
    keys = (HOME + " " + seq).strip()
    reply = fw(ip, f"keys send {keys}")
    if not any(l.strip().startswith("keys sent") for l in reply):
        raise RuntimeError(f"{name}: keys send said: {' / '.join(reply)}")
    lines = fw(ip, "ui overflow")
    head = next((l for l in lines if "ui overflow:" in l), None)
    if head is None:
        raise RuntimeError(f"{name}: no 'ui overflow:' in reply: {' / '.join(lines)}")
    count = int(head.split("ui overflow:")[1].split()[0])
    return count, [l for l in lines if HIT.match(l)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ip")
    ap.add_argument("--only", help="run just the screens whose name contains this")
    ap.add_argument("--list", action="store_true", help="print the screen table and exit")
    args = ap.parse_args()
    if args.list:
        for name, seq in SCREENS:
            print(f"{name:28} {seq}")
        return 0
    hits, errors = 0, 0
    for name, seq in SCREENS:
        if args.only and args.only not in name:
            continue
        try:
            count, detail = check_screen(args.ip, name, seq)
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            print(f"ERROR {name}: {e}")
            errors += 1
            continue
        print(f"{'FAIL' if count else 'ok  '} {name}: {count} overflowing")
        for l in detail:
            print(f"     {l.strip()}")
        hits += count
    # leave the badge on home
    try:
        fw(args.ip, f"keys send {HOME}")
    except (RuntimeError, subprocess.TimeoutExpired):
        pass
    print(f"total: {hits} overflowing label(s), {errors} screen(s) not read")
    return 2 if errors else (1 if hits else 0)


if __name__ == "__main__":
    sys.exit(main())
