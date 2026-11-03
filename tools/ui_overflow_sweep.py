#!/usr/bin/env python3
"""Long-text sweep (#133): walk the badge's screens over Wi-Fi and run "fw ui overflow" on each.

Usage: tools/ui_overflow_sweep.py <badge-ip> [--only NAME] [--list] [--try NAME]
  Runs only the screens marked verified; --try runs one unverified screen so its sequence can be checked and marked.
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

UNVERIFIED: every key sequence below was derived from the code (#141), not run on the badge. Verify on the bench: for each
screen run "--try NAME", look at "fw ui", then set the entry to True. Sequences are tokens of
firmware/components/lichen_shell/ui_keys.h (up down left right enter esc tab bksp wait).

The walk assumes Home's Tiles layout (the default); the sweep reads "fw home" (read only) first and refuses on Grid.
Restoring the layout is a manual step ("fw home layout tiles"): this script never changes a setting.
"""
import argparse
import os
import re
import subprocess
import sys

HOME = "esc esc esc esc esc"  # backs out of any depth; Esc on home does nothing (UNVERIFIED)

# Home remembers its selection (home.c keeps h->sel), so every walk first goes to the top. In Tiles, Up clamps at the
# first tile (home_layout.c move_tiles), 12 ups cover the 11 tiles. Tile order = registration order: MeshCore
# (main.c, before shell_start), then shell_start (shell.c): Settings, About, Files, Keyboard test, Clock, Bench,
# Crash log, Power, Notifications, Release notes.
TILE = {"mesh": 0, "settings": 1, "about": 2, "files": 3, "crashlog": 7, "notifications": 9, "relnotes": 10}


def tile(name):
    """Keys that select a home tile (not open it)."""
    return ["up"] * 12 + ["down"] * TILE[name]


def settings_page(row):
    """Keys that open the Settings row `row` (0-based among the focusable rows; notes are skipped).

    open_settings (app_settings.c) builds the rows in this order, the first one is focused on arrival:
    0 Wi-Fi, 1 Updates over Wi-Fi (switch), 2 Close updates after (cycles), 3 LoRa radio (switch), 4 Airplane mode
    (switch), 5 Display, 6 Time, 7 SD card, 8 Notifications, 9 Battery, 10 Power, 11 Factory reset, 12 About Lichen,
    then the "Apps" rows (the apps with a settings page or an about; MeshCore registers first, so 13 is MeshCore).
    Only the OPEN rows may be passed: the switches (1, 3, 4), the cycler (2) and Factory reset (11) are refused.
    """
    assert row in (0, 5, 6, 7, 8, 9, 10, 12, 13), f"settings row {row} is a switch, a cycler or a risky page"
    return tile("settings") + ["enter"] + ["down"] * row + ["enter"]


def seq(*parts):
    return " ".join(k for part in parts for k in (part if isinstance(part, list) else [part]))


# (name, key sequence from home, verified). A sequence is only run by default once it was walked by hand on the bench and
# set to True (#133 review: a guessed Enter can land on Update now, Send advert or Login). Rule for every entry: the
# LAST Enter opens a page or list, and no Enter is ever sent on a row that switches, transmits or erases: not the
# Settings rows 1-4 and 11, Power > Restart / Power off, Region & radio, TX power, Custom radio, Send advert, a chat
# or a contact. Entries that would need such a row (Settings > MeshCore > Channels is behind "Start/Stop" and
# "Send advert" and moves with the run state; Time > Set date & time edits) are not in the table.
SCREENS = [
    # no keys: the home tile grid itself
    ("home", "", False),
    # tile 1, Enter, Wi-Fi is Settings row 0
    ("settings", seq(tile("settings"), "enter"), False),
    # Settings row 0 (Wi-Fi): Enter opens the Wi-Fi page (open_wifi); the radio switch lives inside it, not touched
    ("settings/wifi", seq(settings_page(0)), False),
    # Settings row 5 (Display): Enter opens Display, its first row is a slider (nothing is pressed on it)
    ("settings/display", seq(settings_page(5)), False),
    # Settings row 6 (Time) opens the page (rows: Set date & time, Time zone)
    ("settings/time", seq(settings_page(6)), False),
    # Time page row 1 (Time zone): Down moves off "Set date & time", Enter opens the zone list (picking needs another Enter)
    ("settings/time/time zone", seq(settings_page(6), "down", "enter"), False),
    # Settings row 7 (SD card) opens sd_screen
    ("settings/sd card", seq(settings_page(7)), False),
    # Settings row 8 (Notifications) opens the settings page of the notices (its rows are switches, not touched)
    ("settings/notifications", seq(settings_page(8)), False),
    # Settings row 9 (Battery)
    ("settings/battery", seq(settings_page(9)), False),
    # Settings row 10 (Power) opens a page whose rows ask a dialog: only the page is opened
    ("settings/power", seq(settings_page(10)), False),
    # Settings row 12 (About Lichen)
    ("settings/about", seq(settings_page(12)), False),
    # Settings row 13: the first app row under "Apps" (MeshCore, registered first): the App settings heading (#141)
    ("settings/apps/meshcore", seq(settings_page(13)), False),
    # Files tile (3). The volume rows are filled by a job: "wait" lets them arrive. Rows: 0 Internal storage, 1 SD card
    ("files", seq(tile("files"), "enter", "wait"), False),
    # Files > SD card (row 1): Down, Enter on the volume row (app_files.c add_volume_rows) opens the listing
    ("files/sd card", seq(tile("files"), "enter", "wait", "down", "enter", "wait"), False),
    # the first row of the SD listing: a folder or a file; both only open (a viewer or a folder), no Backspace or C
    ("files/sd first entry", seq(tile("files"), "enter", "wait", "down", "enter", "wait", "enter", "wait"), False),
    # Crash log tile (7): the list; Enter is not pressed on an entry or on Clear all
    ("crash log", seq(tile("crashlog"), "enter", "wait"), False),
    # Notifications tile (9): the list; Backspace and Enter are not pressed
    ("notify centre", seq(tile("notifications"), "enter", "wait"), False),
    # Release notes tile (10): the list
    ("release notes", seq(tile("relnotes"), "enter", "wait"), False),
    # MeshCore tile (0): the app's grid. Tiles: 0 Chat, 1 Contacts, 2 Repeaters / 3 Heard, 4 Advertise, 5 Settings
    # (meshcore_grid.c, mesh_grid.h; 3 columns, Chat has the focus on arrival). Advertise (4) is never entered. MeshCore must
    # be running: on a stopped MeshCore Enter on any tile but Settings starts it instead (start_instead).
    ("mesh app", seq(tile("mesh"), "enter", "wait"), False),
    ("mesh/chats", seq(tile("mesh"), "enter", "wait", "enter", "wait"), False),
    ("mesh/contacts", seq(tile("mesh"), "enter", "wait", "right", "enter", "wait"), False),
    ("mesh/repeaters", seq(tile("mesh"), "enter", "wait", "right", "right", "enter", "wait"), False),
    ("mesh/heard", seq(tile("mesh"), "enter", "wait", "down", "enter", "wait"), False),
    # Settings tile (5): from Chat (0) Down reaches Heard (3), Right Right reaches Settings (5). Opens the page; no row is pressed
    ("mesh/settings", seq(tile("mesh"), "enter", "wait", "down", "right", "right", "enter", "wait"), False),
]

BADGE_SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "badge.sh")
HIT = re.compile(r"^\s*(-?\d+),(-?\d+) (\d+)/(-?\d+) px ")


def fw(ip, line):
    out = subprocess.run([BADGE_SH, "fw", ip, line], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        raise RuntimeError(f"fw {line!r} failed: {out.stderr.strip() or out.stdout.strip()}")
    return out.stdout.splitlines()


def preflight_tiles(ip):
    """The key walks assume Tiles; "fw home" without arguments only reports the layout."""
    try:
        reply = " ".join(fw(ip, "home"))
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"ERROR reading the home layout: {e}")
        return False
    if "layout is tiles" not in reply:
        print(f"ERROR the walks need the Tiles layout ({reply.strip()}); set it by hand: fw home layout tiles")
        return False
    return True


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
    ap.add_argument("--try", dest="try_name", metavar="NAME",
                    help="run ONE unverified screen (exact name) to check its sequence by hand, then mark it verified")
    args = ap.parse_args()
    if args.list:
        for name, seq, ok in SCREENS:
            print(f"{name:28} {'verified  ' if ok else 'UNVERIFIED'} {seq}")
        return 0
    if not preflight_tiles(args.ip):
        return 2
    hits, errors = 0, 0
    skipped = 0
    for name, seq, ok in SCREENS:
        if args.try_name:
            if name != args.try_name:
                continue
        elif args.only and args.only not in name:
            continue
        elif not ok:
            skipped += 1
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
    print(f"total: {hits} overflowing label(s), {errors} screen(s) not read, {skipped} unverified screen(s) skipped")
    return 2 if errors else (1 if hits else 0)


if __name__ == "__main__":
    sys.exit(main())
