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
  ("focus", text)              the focused row must contain text (never empty; "(none)" and a missing focus never match)
  ("any_row",)                 the first row of a list of contacts or files, which only opens a read-only page: any
                               focused row except a forbidden one (checked against what is actually focused)
Forbidden targets (rows that switch, send, restart or erase) are refused when the table is loaded, and the focused row
is checked against the same list at run time. Only the keys up down left right enter esc wait are ever sent.

A person may be holding the badge: before every key the script compares "fw ui" (screen stack and focus) with what it
saw after its own last action. Any other change stops the whole run with "badge in use: stopped" and sends NO key, not
even Esc. When the run ends any other way (also on Ctrl-C) it sends Esc x5 to leave the badge on home.
"--selftest" feeds fake "fw" replies through this logic (no badge needed).

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
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Sequence, Tuple, Union

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
             "Clear all", "Backup now", "Update now", "Switch radio", "Apply", "Custom", "Save settings",
             "Load settings", "Rename", "Add channel", "Add ")
ALLOWED_KEYS = ("up", "down", "left", "right", "enter", "esc", "wait")


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
     tile("Files") + [("row", "SD card", FILES), "enter", "wait", ("any_row",), "enter", "wait"], False),
    ("clock", (), tile("Clock"), False),
    ("weather", (), tile("Weather"), False),
    ("timer hub", (), tile("Timer"), False),
    ("timer/timer", ("SCREEN_TIMER_PAGE",), tile("Timer") + [("row", "Timer", TIMER), "enter", "wait"], False),
    ("timer/stopwatch", ("SCREEN_STOPWATCH_PAGE",),
     tile("Timer") + [("row", "Stopwatch", TIMER), "enter", "wait"], False),
    ("timer/world clock", ("SCREEN_WORLD_PAGE",),
     tile("Timer") + [("row", "World clock", TIMER), "enter", "wait"], False),
    ("calculator", (), tile("Calculator"), False),
    # Bench starts a run when it opens: wait it out before reading
    ("bench", (), tile("Bench") + ["wait", "wait", "wait"], False),
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
    ("mesh/contact", ("SCREEN_CONTACT",), mc_grid(CONTACTS_TILE) + [("any_row",), "enter", "wait"], False),
    ("mesh/contact signal", ("SCREEN_MC_SIGNAL",),
     mc_grid(CONTACTS_TILE) + [("any_row",), "enter", "wait", ("row", "Signal", CONTACT), "enter", "wait"], False),
    ("mesh/contact share", ("SCREEN_SHARE",),
     mc_grid(CONTACTS_TILE) + [("any_row",), "enter", "wait", ("row", "Share", CONTACT), "enter", "wait"], False),
    ("mesh/contact share qr", ("SCREEN_QR",),
     mc_grid(CONTACTS_TILE) + [("any_row",), "enter", "wait", ("row", "Share", CONTACT), "enter", "wait",
                               ("focus", "Show as QR"), "enter", "wait"], False),
    ("mesh/repeaters", (), mc_grid(REPEATERS_TILE), False),
    ("mesh/repeater page", (), mc_grid(REPEATERS_TILE) + [("any_row",), "enter", "wait"], False),
    ("mesh/heard", (), mc_grid(HEARD_TILE), False),
    ("mesh/nearby", ("SCREEN_MC_DISCOVER",), mc_grid(NEARBY_TILE), False),  # #171: tile position
    ("mesh/radar", ("SCREEN_MC_RADAR",), mc_grid(RADAR_TILE), False),  # #171: tile position
    ("mesh/settings", ("SCREEN_SETTINGS",), mc_grid(SETTINGS_TILE), False),  # #171: tile position
    ("mesh/settings/channels", ("SCREEN_CHANNELS",), mc_settings_page("Channels"), False),
    # the first row of the channel list: a channel (its page) or the add row (a text prompt); neither sends
    ("mesh/settings/channel page", ("SCREEN_CHANNEL",),
     mc_settings_page("Channels") + [("any_row",), "enter", "wait"], False),
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


Step = Union[str, Tuple[Any, ...]]
FwFn = Callable[[str], List[str]]


class InUse(Exception):
    """The badge changed under the script (a person is using it): stop everything, send no key."""


class RouteError(RuntimeError):
    """This screen's route did not go as expected; nothing risky was pressed. The next screen may still run."""


def forbidden(text: str) -> Optional[str]:
    return next((f for f in FORBIDDEN if f in text), None)


def lint_steps(name: str, steps: Sequence[Step]) -> None:
    """Allowlist: only known keys, guarded Enters, no forbidden row, no empty guard."""
    guarded = False
    for st in steps:
        if isinstance(st, tuple):
            kind = st[0]
            if kind == "row":
                if forbidden(st[1]):
                    raise ValueError(f"{name}: row {st[1]!r} is forbidden")
                if st[1] not in st[2]:
                    raise ValueError(f"{name}: row {st[1]!r} is not in its row list")
            elif kind == "focus":
                if not st[1].strip() or st[1] == "(none)":
                    raise ValueError(f"{name}: an empty focus guard")
                if forbidden(st[1]):
                    raise ValueError(f"{name}: focus {st[1]!r} is forbidden")
            elif kind == "fw":
                if not st[1].startswith("qr "):
                    raise ValueError(f"{name}: console command {st[1]!r} is not allowed")
            elif kind != "any_row":
                raise ValueError(f"{name}: unknown step {st!r}")
            guarded = kind in ("row", "focus", "any_row")
        elif st not in ALLOWED_KEYS:
            raise ValueError(f"{name}: key {st!r} is not allowed in the sweep")
        elif st == "enter":
            if not guarded:
                raise ValueError(f"{name}: Enter without a guard in front")
            guarded = False
        elif st != "wait":
            guarded = False


def lint_all() -> None:
    for name, _ids, steps, _ok in SCREENS:
        lint_steps(name, steps)


def screen_id_map() -> Dict[str, int]:
    with open(SCREEN_IDS_H) as f:
        return {n: int(v) for n, v in re.findall(r"X\((SCREEN_\w+),\s*(\d+)\)", f.read())}


def unclassified_ids() -> List[str]:
    covered = {i for _, ids, _, _ in SCREENS for i in ids}
    return [i for i in screen_id_map() if i not in covered and i not in UNREACHABLE]


def run_badge_sh(ip: str, line: str) -> List[str]:
    try:
        out = subprocess.run([BADGE_SH, "fw", ip, line], capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise RouteError(f"fw {line!r} failed: {e}") from e
    if out.returncode != 0:
        raise RouteError(f"fw {line!r} failed: {out.stderr.strip() or out.stdout.strip()}")
    return out.stdout.splitlines()


class Snap(NamedTuple):
    stack: Tuple[int, ...]
    focus: str

    def no_focus(self) -> bool:
        return self.focus in ("", "(none)")


def parse_snapshot(lines: Sequence[str]) -> Snap:
    for line in lines:
        m = re.match(r"\s*ui stack: (.*?)\s*\| focus: (.*)$", line)
        if m:
            return Snap(tuple(int(n) for n in re.findall(r"\d+", m.group(1))), m.group(2).strip())
    raise RouteError(f"unreadable 'fw ui' reply: {' / '.join(lines)[:120]!r}")


def same_row(focus: str, label: str) -> bool:
    return focus.lstrip("* ").strip() == label


class Badge:
    """Drives the badge through `fw`, and never sends a key into a state it did not expect."""

    def __init__(self, fw: FwFn) -> None:
        self.fw = fw
        self.last: Optional[Snap] = None

    def snap(self) -> Snap:
        return parse_snapshot(self.fw("ui"))

    def start(self) -> None:
        self.last = self.snap()

    def _unchanged(self, now: Snap) -> bool:
        last = self.last
        if last is None or now == last:
            return True
        # a list filled by a background job gives the focus to its first row; the screen is the same
        return now.stack == last.stack and last.no_focus() and not now.no_focus()

    def send(self, keys: Sequence[str]) -> None:
        if not keys:
            return
        now = self.snap()
        if not self._unchanged(now):
            raise InUse(f"{self.last} -> {now}")
        reply = self.fw("keys send " + " ".join(keys))
        if not any(l.strip().startswith("keys sent") for l in reply):
            raise RouteError(f"keys send said: {' / '.join(reply)}")
        after = self.snap()
        self.last = after
        if "enter" in keys and len(after.stack) <= len(now.stack):
            raise RouteError(f"Enter opened nothing (stack {list(now.stack)} -> {list(after.stack)})")

    def seek(self, label: str, rows: Sequence[str]) -> None:
        """Down to `label` in `rows`, then check the focus; corrects by single steps (a row that takes no focus)."""
        target = rows.index(label)
        self.send(["down"] * target)
        for _ in range(4):
            cur = self._focus()
            if same_row(cur, label):
                return
            idx = next((i for i, r in enumerate(rows) if same_row(cur, r)), None)
            self.send(["up" if idx is not None and idx > target else "down"])
        if not same_row(self._focus(), label):
            raise RouteError(f"expected focus on {label!r} before Enter, found {self._focus()!r}; nothing pressed on it")

    def _focus(self) -> str:
        if self.last is None:
            raise RouteError("no reading of the screen yet")
        return self.last.focus

    def guard(self, text: str) -> None:
        cur = self._focus()
        if self.last is not None and self.last.no_focus():
            raise RouteError(f"expected focus containing {text!r} before Enter, found no focus; nothing pressed")
        if text not in cur:
            raise RouteError(f"expected focus containing {text!r} before Enter, found {cur!r}; nothing pressed")

    def any_row(self) -> None:
        cur = self._focus()
        if self.last is None or self.last.no_focus():
            raise RouteError("expected a focused row before Enter, found none; nothing pressed")
        bad = forbidden(cur)
        if bad:
            raise RouteError(f"the focused row {cur!r} is forbidden ({bad!r}); nothing pressed")

    def console(self, line: str) -> None:
        now = self.snap()
        if not self._unchanged(now):
            raise InUse(f"{self.last} -> {now}")
        self.fw(line)
        self.last = self.snap()

    def home(self) -> None:
        self.send(HOME.split())

    def walk(self, name: str, steps: Sequence[Step], expect_top: Optional[int] = None) -> None:
        lint_steps(name, steps)
        self.send(HOME.split() + ["wait"])
        pending: List[str] = []
        for st in steps:
            if isinstance(st, str):
                pending.append(st)
                continue
            self.send(pending)
            pending = []
            if st[0] == "row":
                if st[2] is TILES:
                    self.send(["up"] * len(TILES))  # Home remembers its selection: to the top (Up clamps)
                self.seek(st[1], st[2])
            elif st[0] == "focus":
                self.guard(st[1])
            elif st[0] == "any_row":
                self.any_row()
            elif st[0] == "fw":
                self.console(st[1])
        self.send(pending)
        top = self.last.stack[-1] if self.last and self.last.stack else None
        if expect_top is not None and top != expect_top:
            raise RouteError(f"route ended on screen {top}, expected {expect_top}")


def preflight_tiles(ip: str) -> bool:
    """The key walks assume Tiles; "fw home" without arguments only reports the layout."""
    try:
        reply = " ".join(run_badge_sh(ip, "home"))
    except RouteError as e:
        print(f"ERROR reading the home layout: {e}")
        return False
    if "layout is tiles" not in reply:
        print(f"ERROR the walks need the Tiles layout ({reply.strip()}); set it by hand: fw home layout tiles")
        return False
    return True


# console_not_running (mesh_task.cpp) opens a stopped MeshCore's "fw mesh" reply with "MeshCore " and one of these
NOT_RUNNING = ("MeshCore stopped", "MeshCore starting", "MeshCore stopping", "MeshCore not running")


def mesh_running(ip: str) -> bool:
    """False when "fw mesh" (read only) says MeshCore is not running: Enter on a grid tile then STARTS it (start_instead)."""
    try:
        lines = run_badge_sh(ip, "mesh")
    except RouteError:
        return False  # unknown counts as not running
    return bool(lines) and not any(l.strip().startswith(NOT_RUNNING) for l in lines)


def read_overflow(ip: str, name: str) -> Tuple[int, List[str]]:
    lines = run_badge_sh(ip, "ui overflow")
    head = next((l for l in lines if "ui overflow:" in l), None)
    if head is None:
        raise RouteError(f"{name}: no 'ui overflow:' in reply: {' / '.join(lines)}")
    try:
        count = int(head.split("ui overflow:")[1].split()[0])
    except (ValueError, IndexError) as e:
        raise RouteError(f"{name}: unreadable count in {head!r}") from e
    return count, [l for l in lines if HIT.match(l) or l.strip().startswith("...")]


def print_list() -> int:
    lint_all()
    for name, ids, _steps, ok in SCREENS:
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


# ---- --selftest: fake "fw" replies through the guard logic ------------------------------------------------------------

class FakeFw:
    """Answers `fw ui` from a queue (the last one repeats) and records every `keys send`."""

    def __init__(self, ui_replies: Sequence[Sequence[str]]) -> None:
        self.ui = [list(r) for r in ui_replies]
        self.sent: List[str] = []

    def __call__(self, line: str) -> List[str]:
        if line.startswith("keys send"):
            self.sent.append(line)
            return ["keys sent: 1"]
        if line == "ui":
            return self.ui.pop(0) if len(self.ui) > 1 else list(self.ui[0])
        return []


def ui(stack: str, focus: str) -> List[str]:
    return [f"ui stack: {stack} | focus: {focus}", "ui   x"]


def selftest() -> int:
    failures: List[str] = []

    def expect(label: str, exc: Optional[type], fake: FakeFw, act: Callable[[Badge], None], keys_sent: int) -> None:
        badge = Badge(fake)
        try:
            badge.start()
            act(badge)
            got = None
        except (InUse, RouteError) as e:
            got = type(e)
        if got is not exc or len(fake.sent) != keys_sent:
            failures.append(f"{label}: raised {got}, keys sent {len(fake.sent)} (wanted {exc}, {keys_sent})")

    expect("empty reply", RouteError, FakeFw([[]]), lambda b: b.send(["down"]), 0)
    expect("garbled reply", RouteError, FakeFw([["garbage", "ui stack"]]), lambda b: b.send(["down"]), 0)
    expect("no focus line", RouteError, FakeFw([["ui stack: 1 > 3"]]), lambda b: b.send(["down"]), 0)
    expect("(none) at a guard", RouteError, FakeFw([ui("1 > 3", "(none)")]), lambda b: b.guard("Display"), 0)
    expect("empty focus at any_row", RouteError, FakeFw([ui("1 > 3", "")]), lambda b: b.any_row(), 0)
    expect("forbidden focus at any_row", RouteError, FakeFw([ui("1 > 3", "Remove")]), lambda b: b.any_row(), 0)
    expect("wrong row", RouteError, FakeFw([ui("1 > 3", "Time")]), lambda b: b.guard("Display"), 0)
    expect("dialog appeared", InUse, FakeFw([ui("1 > 3", "Display"), ui("1 > 3", "Cancel")]),
           lambda b: b.send(["enter"]), 0)
    expect("stack changed", InUse, FakeFw([ui("1 > 3", "Display"), ui("1", "Settings")]),
           lambda b: b.send(["enter"]), 0)
    expect("focus moved by a person", InUse, FakeFw([ui("1 > 3", "Display"), ui("1 > 3", "Time")]),
           lambda b: b.send(["down"]), 0)
    expect("list filled by a job", None, FakeFw([ui("1 > 3", "(none)"), ui("1 > 3", "SD card"),
                                                       ui("1 > 3", "SD card")]), lambda b: b.send(["down"]), 1)
    expect("enter opens a page", None, FakeFw([ui("1", "Settings"), ui("1", "Settings"), ui("1 > 100", "Wi-Fi")]),
           lambda b: b.send(["enter"]), 1)
    expect("enter opens nothing", RouteError, FakeFw([ui("1", "Settings")]), lambda b: b.send(["enter"]), 1)
    for bad in ([("focus", "")], ["bksp"], [("focus", "Remove")], ["enter"], [("row", "Wi-Fi", ["Wi-Fi"]), "tab"]):
        try:
            lint_steps("selftest", bad)
            failures.append(f"lint accepted {bad!r}")
        except ValueError:
            pass
    lint_all()
    if failures:
        print("\n".join("selftest FAIL " + f for f in failures))
        return 1
    print("selftest: ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("ip", nargs="?")
    ap.add_argument("--only", help="run just the screens whose name contains this")
    ap.add_argument("--all", action="store_true", help="also run the screens not marked verified")
    ap.add_argument("--list", action="store_true", help="print the screen table and the unreachable screens, then exit")
    ap.add_argument("--selftest", action="store_true", help="check the guard logic against fake replies, then exit")
    ap.add_argument("--try", dest="try_name", metavar="NAME", help="run ONE screen (exact name) to check its route")
    args = ap.parse_args()
    try:
        lint_all()
    except ValueError as e:
        print(f"ERROR route table: {e}")
        return 2
    if args.selftest:
        return selftest()
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
    return run(args)


def run(args: argparse.Namespace) -> int:
    ip = args.ip
    ids = screen_id_map()
    badge = Badge(lambda line: run_badge_sh(ip, line))
    hits = errors = skipped = ran = 0
    kinds: Dict[str, int] = {k: 0 for k in KINDS}
    in_use = False
    try:
        badge.start()
        for name, sids, steps, ok in SCREENS:
            if args.try_name:
                if name != args.try_name:
                    continue
            elif args.only and args.only not in name:
                continue
            elif not ok and not args.all:
                skipped += 1
                continue
            # Enter on a grid tile of a stopped MeshCore would START it: check right before every mesh walk
            if name.startswith("mesh") and not mesh_running(ip):
                print(f"skip  {name}: MeshCore is not running (start it by hand)")
                skipped += 1
                continue
            try:
                badge.walk(name, steps, ids[sids[0]] if len(sids) == 1 else None)
                count, detail = read_overflow(ip, name)
            except RouteError as e:
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
    except InUse as e:
        in_use = True
        print(f"badge in use: stopped ({e})")
    finally:
        if not in_use:  # also on Ctrl-C: leave the badge on home
            try:
                badge.home()
            except (InUse, RouteError):
                print("badge in use: stopped (no key sent to go home)")
                in_use = True
    by_kind = ", ".join(f"{k} {n}" for k, n in kinds.items())
    print(f"total: {hits} hit(s) ({by_kind}), {ran} screen(s) read, {errors} not read, {skipped} skipped")
    print(f"not reachable: {len(UNREACHABLE) + len(UNREACHABLE_APPS)} (python3 tools/ui_overflow_sweep.py --list)")
    return 2 if (errors or in_use) else (1 if hits else 0)


if __name__ == "__main__":
    sys.exit(main())
