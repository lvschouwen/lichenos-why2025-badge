#!/usr/bin/env python3
"""Long-text and layout sweep (#133): walk the badge's screens over Wi-Fi and run "fw ui overflow" on each.

Usage: tools/ui_overflow_sweep.py <badge-ip> [--all] [--only NAME] [--list] [--try NAME]
  Default: only the screens marked verified (none yet, see below). --all runs every reachable screen, --try runs one.
  --list prints the table, the screens it cannot reach and any screen id in screen_ids.h that is in neither.
  Exit 0: every screen clean. 1: at least one hit. 2: a screen could not be driven or read, or the table is stale.
  Needs the OTA token like tools/badge.sh (see its header). Talks to the badge only through "badge.sh fw <ip> ...".

What "fw ui overflow" reports (one line per hit, kinds): width (text cut off sideways), height (wrapped text taller than
its fixed label), clipped (an object that no scroll position shows: off the screen or beyond a parent), under-hint (an
object that runs under the pinned key hint or the status bar). This script only counts the lines; a hit is a bug in the
screen, not in the script.

What it sends: "fw keys send <seq>" (a fresh walk from home for every screen), "fw ui" (read only) and, for the QR
screen, "fw qr <text>" (shows a code, sends nothing). Never "text:" tokens, nothing that transmits or changes a setting.

How the walks stay safe: every Enter is directly preceded by a guard. A guard reads "fw ui" and compares the focused row
with the one the route expects; when it differs, the screen is reported as an error and the Enter is NOT sent. So a stale
route (the home tiles or a menu changed) can open the wrong page at worst, never press a switch. Steps:
  "up" "enter" "wait" ...      key tokens of firmware/components/lichen_shell/ui_keys.h
  ("row", label, ROWS)         move down to row `label` of the list ROWS (the labels of the focusable rows in order),
                               correcting by single steps if the page differs; then the focus must equal the label
  ("focus", text)              the focused row must contain text ("" accepts whatever is focused: used for the first
                               row of a list of contacts or files, which only opens a read-only page)
Forbidden targets (rows that switch, send, restart or erase) are refused when the table is loaded.

Fixtures (set up by hand first; bench rules: on-air targets are ONLY #badgetest and the Heltec test node, never real
contacts; this script sends nothing on air):
  - a channel with a 31-character name (a #badgetest-style channel, e.g. "#badgetest-0123456789-abcdefghij"), first in
    the chat list
  - the Heltec test node renamed to a 32-character name, heard so it is in the contact list (and a repeater entry)
  - a file with a 127-character name on the SD card (and one long folder name) in the root Files shows first
  - a saved Wi-Fi network with a 32-character SSID in range or in the saved list
  - a few notifications in the centre with long text (a long channel-name message from #badgetest)
Without the fixtures the sweep still runs but only proves short data fits.

UNVERIFIED: no route below has been walked on the badge yet (written from the code, #133). Run "--try NAME" for one,
look at "fw ui", then set its flag to True.

The walk assumes Home's Tiles layout (the default); the sweep reads "fw home" (read only) first and refuses on Grid.
Restoring the layout is a manual step ("fw home layout tiles"): this script never changes a setting.
"""
import argparse
import os
import re
import subprocess
import sys

HOME = "esc esc esc esc esc"  # backs out of any depth; Esc on home does nothing

# Home tiles in order: MeshCore (main.c, before shell_start), then shell_start's register calls (shell.c): Settings,
# About, Files, Keyboard test, Clock, Weather, Timer, Calculator, Bench, Crash log, Power, Notifications, What's new,
# Notes, QR code, LoRa spectrum. The label is the app's name (home_selected_name).
TILES = ["MeshCore", "Settings", "About Lichen", "Files", "Keyboard test", "Clock", "Weather", "Timer", "Calculator",
         "Bench", "Crash log", "Power", "Notifications", "What's new", "Notes", "QR code", "LoRa spectrum"]
# Settings (app_settings.c open_settings): the focusable rows in order. "Name on the network" has no action; if it takes
# no focus the "row" step corrects by one. PSRAM speed is only there when supported; the Apps rows follow.
SETTINGS = ["Wi-Fi", "Updates over Wi-Fi", "Name on the network", "Close updates after", "LoRa radio", "Airplane mode",
            "Display", "Time", "SD card", "Notifications", "Battery", "Power", "Backup", "Factory reset",
            "About Lichen", "Console", "PSRAM speed", "MeshCore"]
TIME = ["Set date & time", "Time zone"]
TIMER = ["Timer", "Stopwatch", "World clock"]
NOTES = ["New note...", "New to do..."]
FILES = ["Internal storage", "SD card"]
# MeshCore > Settings (meshcore_settings.c meshcore_settings) AFTER #171 moved Discover nearby and Contact radar to the
# grid. The run row's text changes with the run state, so it is a placeholder.
MC_SETTINGS = ["Start at boot", "(run)", "Name", "Region & radio", "Flood scope", "Channels", "Send advert",
               "Notifications", "Advert & contacts", "Radio health", "Air log"]
# A contact's page (meshcore_contact.c): the rows of a normal contact (a repeater has Log in, Status, ... instead)
CONTACT = ["Message", "Reset path", "Signal", "Request telemetry", "Trace path", "Discover path", "Favourite", "Share",
           "Remove"]

# Rows that switch, send, restart or erase: a route may never end in Enter on one of these
FORBIDDEN = ("Updates over", "Close updates", "LoRa radio", "Airplane", "PSRAM", "Region & radio", "Send advert",
             "Advertise", "Start at boot", "(run)", "Request telemetry", "Trace path", "Discover path", "Reset path",
             "Remove", "Favourite", "Message", "Log in", "Log out", "Forget", "Restart", "Power off", "Delete",
             "Clear all", "Backup now", "Update now")


def tile(name):
    return [("row", name, TILES), "enter", "wait"]


def settings_page(label):
    return tile("Settings") + [("row", label, SETTINGS), "enter", "wait"]


def mc_settings_page(label):
    return mc_grid(SETTINGS_TILE) + [("row", label, MC_SETTINGS), "enter", "wait"]


# MeshCore grid AFTER #171 (mesh_grid.h): 3 columns, Chat has the focus on arrival.
#   Chat(0) Contacts(1) Repeaters(2) / Heard(3) Nearby(4) Radar(5) / Advertise(6) Settings(7)
# The moves below are for that grid; each ends in a focus guard, so on the old 6-tile grid they stop before the Enter.
CHAT_TILE = ([], "Chat")
CONTACTS_TILE = (["right"], "Contacts")
REPEATERS_TILE = (["right", "right"], "Repeaters")
HEARD_TILE = (["down"], "Heard")
NEARBY_TILE = (["down", "right"], "Nearby")  # #171: adjust if the tile order changes
RADAR_TILE = (["down", "right", "right"], "Radar")  # #171: adjust if the tile order changes
SETTINGS_TILE = (["down", "down", "right"], "Settings")  # #171: was down right right on the 6-tile grid


def mc_grid(tile_moves=None):
    """Open MeshCore; with tile_moves = (keys, label) also move to that tile and open it."""
    steps = tile("MeshCore")
    if tile_moves:
        keys, label = tile_moves
        steps += list(keys) + [("focus", label), "enter", "wait"]
    return steps


# (name, screen ids it covers, steps from home, verified). Rule for every entry: the LAST Enter opens a page or list and
# is preceded by a guard; no Enter on a switch, a sender, Region & radio, TX power, Advertise, a DM or a compose
# screen. Typing ("text:") is never used.
SCREENS = [
    ("home", (), [], False),
    ("settings", (), tile("Settings"), False),
    ("settings/wifi", ("SCREEN_WIFI",), settings_page("Wi-Fi"), False),
    ("settings/display", ("SCREEN_DISPLAY",), settings_page("Display"), False),
    ("settings/time", ("SCREEN_TIME",), settings_page("Time"), False),
    ("settings/time/time zone", ("SCREEN_TZ",),
     settings_page("Time") + [("row", "Time zone", TIME), "enter", "wait"], False),
    ("settings/time/set date", ("SCREEN_TIME_SET",),
     settings_page("Time") + [("row", "Set date & time", TIME), "enter", "wait"], False),
    ("settings/sd card", ("SCREEN_SD",), settings_page("SD card"), False),
    ("settings/notifications", ("SCREEN_NOTIFY_SETTINGS",), settings_page("Notifications"), False),
    ("settings/battery", ("SCREEN_BATTERY",), settings_page("Battery"), False),
    ("settings/power", (), settings_page("Power"), False),
    ("settings/backup", ("SCREEN_BACKUP",), settings_page("Backup"), False),
    ("settings/factory reset", ("SCREEN_FACTORY",), settings_page("Factory reset"), False),
    ("settings/about", ("SCREEN_ABOUT",), settings_page("About Lichen"), False),
    ("settings/console", ("SCREEN_CONSOLE",), settings_page("Console"), False),
    ("settings/apps/meshcore", (), settings_page("MeshCore"), False),
    ("about tile", (), tile("About Lichen"), False),
    ("files", (), tile("Files"), False),
    ("files/internal", (), tile("Files") + [("row", "Internal storage", FILES), "enter", "wait"], False),
    ("files/sd card", (), tile("Files") + [("row", "SD card", FILES), "enter", "wait"], False),
    # first row of the SD listing: a folder or a file; both only open (a viewer or a folder), no Backspace or C
    ("files/sd first entry", ("SCREEN_FILES_VIEW",),
     tile("Files") + [("row", "SD card", FILES), "enter", "wait", ("focus", ""), "enter", "wait"], False),
    ("clock", (), tile("Clock"), False),
    ("weather", (), tile("Weather"), False),
    ("timer hub", (), tile("Timer"), False),
    ("timer/timer", ("SCREEN_TIMER_PAGE",), tile("Timer") + [("row", "Timer", TIMER), "enter", "wait"], False),
    ("timer/stopwatch", ("SCREEN_STOPWATCH_PAGE",),
     tile("Timer") + [("row", "Stopwatch", TIMER), "enter", "wait"], False),
    ("timer/world clock", ("SCREEN_WORLD_PAGE",),
     tile("Timer") + [("row", "World clock", TIMER), "enter", "wait"], False),
    ("calculator", (), tile("Calculator"), False),
    ("bench", (), tile("Bench"), False),
    ("crash log", (), tile("Crash log"), False),
    ("power", (), tile("Power"), False),
    ("notify centre", (), tile("Notifications"), False),
    ("release notes", (), tile("What's new"), False),
    ("notes", (), tile("Notes"), False),
    # "New note..." opens the text prompt (nothing typed, so nothing saved; Esc leaves)
    ("notes/new note prompt", ("SCREEN_TEXT_ENTRY",),
     tile("Notes") + [("row", "New note...", NOTES), "enter", "wait"], False),
    ("qr list", (), tile("QR code"), False),
    # a QR code for a 100-character text, shown by the console (a code is a picture: the check is the caption and hint)
    ("qr/code", ("SCREEN_QR",), [("fw", "qr " + "0123456789" * 10), "wait"], False),
    # opens the spectrum: MeshCore pauses and the radio only listens; Enter (other band) and Backspace are not pressed
    ("lora spectrum", (), tile("LoRa spectrum") + ["wait", "wait"], False),
    # Keyboard test is not here: leaving it needs Esc held for 2 s, which "keys send" cannot do (see UNREACHABLE)
    ("mesh app", (), mc_grid(), False),
    ("mesh/chats", ("SCREEN_CHATS",), mc_grid(CHAT_TILE), False),
    # the chat list's first row must be the #badgetest channel (fixture); any other first row aborts before Enter
    ("mesh/chat #badgetest", ("SCREEN_CHAT",), mc_grid(CHAT_TILE) + [("focus", "#badgetest"), "enter", "wait"], False),
    ("mesh/contacts", ("SCREEN_CONTACTS",), mc_grid(CONTACTS_TILE), False),
    ("mesh/contact", ("SCREEN_CONTACT",), mc_grid(CONTACTS_TILE) + [("focus", ""), "enter", "wait"], False),
    ("mesh/contact signal", ("SCREEN_MC_SIGNAL",),
     mc_grid(CONTACTS_TILE) + [("focus", ""), "enter", "wait", ("row", "Signal", CONTACT), "enter", "wait"], False),
    ("mesh/contact share", ("SCREEN_SHARE",),
     mc_grid(CONTACTS_TILE) + [("focus", ""), "enter", "wait", ("row", "Share", CONTACT), "enter", "wait"], False),
    ("mesh/contact share qr", ("SCREEN_QR",),
     mc_grid(CONTACTS_TILE) + [("focus", ""), "enter", "wait", ("row", "Share", CONTACT), "enter", "wait",
                               ("focus", "Show as QR"), "enter", "wait"], False),
    ("mesh/repeaters", (), mc_grid(REPEATERS_TILE), False),
    ("mesh/repeater page", (), mc_grid(REPEATERS_TILE) + [("focus", ""), "enter", "wait"], False),
    ("mesh/heard", (), mc_grid(HEARD_TILE), False),
    ("mesh/nearby", ("SCREEN_MC_DISCOVER",), mc_grid(NEARBY_TILE), False),  # #171: tile position
    ("mesh/radar", ("SCREEN_MC_RADAR",), mc_grid(RADAR_TILE), False),  # #171: tile position
    ("mesh/settings", ("SCREEN_SETTINGS",), mc_grid(SETTINGS_TILE), False),  # #171: tile position
    ("mesh/settings/channels", ("SCREEN_CHANNELS",), mc_settings_page("Channels"), False),
    # the first row of the channel list: a channel (its page) or the add row (a text prompt); neither sends
    ("mesh/settings/channel page", ("SCREEN_CHANNEL",),
     mc_settings_page("Channels") + [("focus", ""), "enter", "wait"], False),
    ("mesh/settings/notifications", ("SCREEN_NOTIFY_PAGE",), mc_settings_page("Notifications"), False),
    ("mesh/settings/advert contacts", ("SCREEN_ADVERT",), mc_settings_page("Advert & contacts"), False),
    ("mesh/settings/radio health", ("SCREEN_MC_HEALTH",), mc_settings_page("Radio health"), False),
    ("mesh/settings/air log", ("SCREEN_MC_AIRLOG",), mc_settings_page("Air log"), False),
]

# What the sweep cannot reach by keys without sending, switching or needing data that is not there. Every id in
# screen_ids.h is either covered above or listed here (checked on every run).
UNREACHABLE = {
    "SCREEN_WELCOME": "first-run only; showing it again needs 'fw setup reset' (changes state)",
    "SCREEN_WIFI_ANALYSER": "its row sits after the scanned networks, so its position is not fixed",
    "SCREEN_CRASH_ENTRY": "needs a crash entry; Enter on the crash list's first row is Clear all when it is empty",
    "SCREEN_NOTIFY_DETAIL": "needs a notice in the centre, and the list may start with Clear all",
    "SCREEN_RADIO": "behind Region & radio: Enter there is forbidden",
    "SCREEN_CUSTOM": "behind Region & radio: Enter there is forbidden",
    "SCREEN_SWITCH": "the radio-switch confirm, opened from Region & radio",
    "SCREEN_DM_PICK": "starts a message to a chosen contact",
    "SCREEN_DM": "a message to a contact; only #badgetest or the Heltec node may be written to, by hand",
    "SCREEN_UPDATE": "Update now flashes the badge",
    "SCREEN_POWER_BYE": "the power-off screen",
    "SCREEN_TRACE": "a trace sends on air",
    "SCREEN_LOGIN": "the repeater login page; reached through 'Log in' (sends on Enter)",
    "SCREEN_STATUS": "needs a repeater login (transmits)",
    "SCREEN_CLI": "needs a repeater login (transmits)",
    "SCREEN_RSET": "needs a repeater login (transmits)",
    "SCREEN_TELEMETRY": "building the page sends a telemetry request",
    "SCREEN_NOTES_ENTRY": "needs a release note row that is not the 'This is new' page; list only",
    "SCREEN_NOTES_NEW": "the 'This is new' page after an update; shows once",
    "SCREEN_NOTE_ITEM": "needs a saved note on the SD card",
    "SCREEN_FLASHLIGHT": "a white screen at full brightness: nothing to measure, a key press ends it",
    "SCREEN_KEYTEST": "Keyboard test ends only on Esc held 2 s, which 'keys send' cannot do",
}
# Screens that have no id of their own (the apps' first screens) and cannot be reached either
UNREACHABLE_APPS = {
    "Keyboard test": UNREACHABLE["SCREEN_KEYTEST"],
}
UNREACHABLE.pop("SCREEN_KEYTEST")

BADGE_SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "badge.sh")
SCREEN_IDS_H = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "components", "lichen_api",
                            "screen_ids.h")
HIT = re.compile(r"^\s*(width|height|clipped|under-hint) (-?\d+),(-?\d+) ")
KINDS = ("width", "height", "clipped", "under-hint")


def lint_steps(name, steps):
    """Refuses a table that could press a forbidden row or an Enter without a guard in front."""
    guarded = False
    for st in steps:
        if isinstance(st, tuple):
            if st[0] == "row":
                if any(f in st[1] for f in FORBIDDEN):
                    raise ValueError(f"{name}: row {st[1]!r} is forbidden")
                if st[1] not in st[2]:
                    raise ValueError(f"{name}: row {st[1]!r} is not in its row list")
            if st[0] == "fw" and not st[1].startswith("qr "):
                raise ValueError(f"{name}: console command {st[1]!r} is not allowed")
            guarded = st[0] in ("row", "focus")
        elif st == "enter":
            if not guarded:
                raise ValueError(f"{name}: Enter without a guard in front")
            guarded = False
        elif st.startswith("text:") or st == "bksp":
            raise ValueError(f"{name}: {st!r} is not allowed in the sweep")
        elif st != "wait":
            guarded = False


def screen_ids_in_header():
    with open(SCREEN_IDS_H) as f:
        return re.findall(r"X\((SCREEN_\w+),\s*\d+\)", f.read())


def unclassified_ids():
    covered = {i for _, ids, _, _ in SCREENS for i in ids}
    return [i for i in screen_ids_in_header() if i not in covered and i not in UNREACHABLE]


def fw(ip, line):
    out = subprocess.run([BADGE_SH, "fw", ip, line], capture_output=True, text=True, timeout=120)
    if out.returncode != 0:
        raise RuntimeError(f"fw {line!r} failed: {out.stderr.strip() or out.stdout.strip()}")
    return out.stdout.splitlines()


def send_keys(ip, keys):
    if not keys:
        return
    reply = fw(ip, "keys send " + " ".join(keys))
    if not any(l.strip().startswith("keys sent") for l in reply):
        raise RuntimeError(f"keys send said: {' / '.join(reply)}")


def focus_of(ip):
    for l in fw(ip, "ui"):
        m = re.search(r"focus: (.*)$", l)
        if m:
            return m.group(1).strip()
    return ""


def same_row(focus, label):
    return focus.lstrip("* ").strip() == label


def seek(ip, label, rows):
    """Moves down to `label` in `rows` and checks the focus; corrects by single steps (a row that takes no focus)."""
    target = rows.index(label)
    send_keys(ip, ["down"] * target)
    for _ in range(4):
        cur = focus_of(ip)
        if same_row(cur, label):
            return
        idx = next((i for i, r in enumerate(rows) if same_row(cur, r)), None)
        send_keys(ip, ["up" if idx is not None and idx > target else "down"])
    cur = focus_of(ip)
    if not same_row(cur, label):
        raise RuntimeError(f"expected focus on {label!r} before Enter, found {cur!r}; nothing was pressed on it")


def walk(ip, name, steps):
    """Home first, then the steps; guards read the focus before every Enter."""
    lint_steps(name, steps)
    send_keys(ip, HOME.split() + ["wait"])
    pending = []
    for st in steps:
        if isinstance(st, str):
            pending.append(st)
            continue
        send_keys(ip, pending)
        pending = []
        if st[0] == "row":
            if st[2] is TILES:
                send_keys(ip, ["up"] * len(TILES))  # Home remembers its selection: to the top (Up clamps)
            seek(ip, st[1], st[2])
        elif st[0] == "focus":
            cur = focus_of(ip)
            if st[1] not in cur:
                raise RuntimeError(f"expected focus containing {st[1]!r} before Enter, found {cur!r}; nothing was pressed")
        elif st[0] == "fw":
            fw(ip, st[1])
    send_keys(ip, pending)


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


# console_not_running (mesh_task.cpp) opens a stopped MeshCore's "fw mesh" reply with "MeshCore " and one of these
NOT_RUNNING = ("MeshCore stopped", "MeshCore starting", "MeshCore stopping", "MeshCore not running")


def mesh_running(ip):
    """False when "fw mesh" (read only) says MeshCore is not running: Enter on a grid tile then STARTS it (start_instead)."""
    try:
        lines = fw(ip, "mesh")
    except (RuntimeError, subprocess.TimeoutExpired):
        return False  # unknown counts as not running: the mesh/* walks are skipped
    return not any(l.strip().startswith(NOT_RUNNING) for l in lines)


def read_overflow(ip, name):
    lines = fw(ip, "ui overflow")
    head = next((l for l in lines if "ui overflow:" in l), None)
    if head is None:
        raise RuntimeError(f"{name}: no 'ui overflow:' in reply: {' / '.join(lines)}")
    count = int(head.split("ui overflow:")[1].split()[0])
    return count, [l for l in lines if HIT.match(l) or l.strip().startswith("...")]


def check_screen(ip, name, steps):
    walk(ip, name, steps)
    return read_overflow(ip, name)


def print_list():
    for name, ids, steps, ok in SCREENS:
        lint_steps(name, steps)
        print(f"{name:32} {'verified  ' if ok else 'UNVERIFIED'} {' '.join(ids)}")
    print("\nnot reachable by keys without sending or switching:")
    for sid, why in sorted(UNREACHABLE.items()):
        print(f"  {sid:24} {why}")
    for app, why in UNREACHABLE_APPS.items():
        print(f"  {app:24} {why}")
    stale = unclassified_ids()
    if stale:
        print(f"\nUNCLASSIFIED in screen_ids.h (add a route or an UNREACHABLE reason): {', '.join(stale)}")
    return 2 if stale else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ip", nargs="?")
    ap.add_argument("--only", help="run just the screens whose name contains this")
    ap.add_argument("--all", action="store_true", help="also run the screens not marked verified")
    ap.add_argument("--list", action="store_true", help="print the screen table and the unreachable screens, then exit")
    ap.add_argument("--try", dest="try_name", metavar="NAME", help="run ONE screen (exact name) to check its route")
    args = ap.parse_args()
    if args.list:
        return print_list()
    if not args.ip:
        ap.error("the badge address is required")
    stale = unclassified_ids()
    if stale:
        print(f"ERROR screen_ids.h has ids this script knows nothing about: {', '.join(stale)}")
        return 2
    if not preflight_tiles(args.ip):
        return 2
    mesh_ok = mesh_running(args.ip)
    if not mesh_ok:
        print("note: MeshCore is not running (or unreadable): the mesh app entries are skipped; start it by hand first")
    hits, errors, skipped, ran = 0, 0, 0, 0
    kinds = dict.fromkeys(KINDS, 0)
    for name, _ids, steps, ok in SCREENS:
        if args.try_name:
            if name != args.try_name:
                continue
        elif args.only and args.only not in name:
            continue
        elif not ok and not args.all:
            skipped += 1
            continue
        if name.startswith("mesh") and not mesh_ok:
            skipped += 1
            continue
        try:
            count, detail = check_screen(args.ip, name, steps)
        except (RuntimeError, ValueError, subprocess.TimeoutExpired) as e:
            print(f"ERROR {name}: {e}")
            errors += 1
            continue
        ran += 1
        print(f"{'FAIL' if count else 'ok  '} {name}: {count} hit(s)")
        for l in detail:
            print(f"     {l.strip()}")
            m = HIT.match(l)
            if m:
                kinds[m.group(1)] += 1
        hits += count
    try:  # leave the badge on home
        send_keys(args.ip, HOME.split())
    except (RuntimeError, subprocess.TimeoutExpired):
        pass
    by_kind = ", ".join(f"{k} {n}" for k, n in kinds.items())
    print(f"total: {hits} hit(s) ({by_kind}), {ran} screen(s) read, {errors} not read, {skipped} skipped")
    print(f"not reachable: {len(UNREACHABLE) + len(UNREACHABLE_APPS)} (python3 tools/ui_overflow_sweep.py --list)")
    return 2 if errors else (1 if hits else 0)


if __name__ == "__main__":
    sys.exit(main())
