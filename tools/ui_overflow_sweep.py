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

What it sends: "fw ui lock on" at the start and "fw ui lock off" at the end (#190: the badge ignores its own keyboard
meanwhile and shows "Remote control: keys locked"), "fw ui open <app>" (the app as from its home tile, or home),
"fw keys send <seq>" (one per route), "fw ui" (read only) and, for the QR screen, "fw qr <text>" (shows a code, sends
nothing). Never "text:" tokens, nothing that transmits or changes a setting.

How the walks stay safe: every Enter is directly preceded by a guard. The guards go in the same "keys send" as the keys
(focus:<text> / row:<text>, ui_keys.h): the badge compares the focused row with the one the route expects right before
the Enter, and when it differs stops the sequence there, so the Enter is NOT sent and the screen is reported as an
error (a row guard first gets the old correction: single steps, each read back with "fw ui"). So a stale route (a menu
changed) can open the wrong page at worst, never press a switch. One route costs about five requests (read, open, keys,
read, overflow) instead of three per key group. Steps:
  "up" "enter" "wait" ...      key tokens of firmware/components/lichen_shell/ui_keys.h
  ("row", label, ROWS)         move down to row `label` of the list ROWS (the labels of the focusable rows in order),
                               correcting by single steps if the page differs; then the focus must equal the label
  ("focus", text)              the focused row must contain text (never empty; "(none)" and a missing focus never match)
  ("any_row",)                 the first row of a list of contacts or files, which only opens a read-only page: any
                               focused row except a forbidden one (checked against what is actually focused)
Forbidden targets (rows that switch, send, restart or erase) are refused when the table is loaded, and the focused row
is checked against the same list at run time. Only the keys up down left right enter esc wait are ever sent.

A person may be holding the badge: the key lock keeps their keys out, and they can take the badge back by holding Esc
for 3 s (the lock also ends 60 s after the last command, so a dead script never leaves the badge deaf). Before every
route the script compares "fw ui" (screen stack and focus) with what it saw after its own last action, and checks the
lock still holds; a refused lock, an ended lock or any other change stops the whole run with "badge in use: stopped"
and sends NO key. A sequence the lock ended under stops on the badge itself. When the run ends any other way (also on
Ctrl-C) it opens home; the lock is always released at the end.
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

Apps are opened by name ("fw ui open"), so Home's layout and order no longer matter.
"""
import argparse
import os
import re
import subprocess
import sys
from typing import Any, Callable, Dict, List, NamedTuple, Optional, Sequence, Tuple, Union

# The apps "fw ui open" takes by name (lichen_app_t.name: MeshCore from main.c, the rest from shell_start, shell.c)
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
HOME_APP = "home"  # "fw ui open home": back to home from any depth


def tile(name):
    """The app as from its home tile: "fw ui open <name>" (#190)."""
    return [("open", name)]


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
    for i, st in enumerate(steps):
        if isinstance(st, tuple):
            kind = st[0]
            if kind in ("row", "focus") and "|" in st[1]:
                raise ValueError(f"{name}: a guard text may not hold '|'")
            if kind == "open":
                if i != 0:
                    raise ValueError(f"{name}: open is only the first step")
                if st[1] not in TILES:
                    raise ValueError(f"{name}: open {st[1]!r} is no known app")
            elif kind == "row":
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
            guarded = kind in ("row", "focus", "any_row")  # open: a fresh screen, no row checked yet
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


def parse_lock(lines: Sequence[str]) -> Optional[bool]:
    """The "ui lock: on, N s left" / "ui lock: off" line of "fw ui" (#190); None when there is none."""
    for line in lines:
        m = re.match(r"\s*ui lock: (on|off)\b", line)
        if m:
            return m.group(1) == "on"
    return None


def same_row(focus: str, label: str) -> bool:
    return focus.lstrip("* ").strip() == label


class Guard(NamedTuple):
    """A focus check the badge runs inside "keys send" right before the next key (ui_keys.h)."""
    kind: str  # "row": the focus is label; "focus": the focus contains label
    label: str
    rows: Sequence[str] = ()

    def token(self) -> str:
        return f"{self.kind}:{self.label}|"


Token = Union[str, Guard]
GUARD_FAILED = re.compile(r"\s*keys send: guard (\d+) failed: focus '(.*)';")


class Badge:
    """Drives the badge through `fw`, and never sends a key into a state it did not expect."""

    def __init__(self, fw: FwFn) -> None:
        self.fw = fw
        self.last: Optional[Snap] = None
        self.locked: Optional[bool] = None  # what the last "fw ui" said about the key lock

    def snap(self) -> Snap:
        lines = self.fw("ui")
        snap = parse_snapshot(lines)
        self.locked = parse_lock(lines)
        return snap

    def start(self) -> None:
        self.last = self.snap()

    def lock(self) -> None:
        """The badge ignores its own keyboard until unlock (#190). A refusal stops the run before any key."""
        reply = self.fw("ui lock on")
        if not any(l.strip().startswith("ui lock: on") for l in reply):
            raise InUse(f"the key lock was refused: {' / '.join(reply)[:120]}")

    def unlock(self) -> None:
        self.fw("ui lock off")

    def _unchanged(self, now: Snap) -> bool:
        last = self.last
        if last is None or now == last:
            return True
        # a list filled by a background job gives the focus to its first row; the screen is the same
        return now.stack == last.stack and last.no_focus() and not now.no_focus()

    def _check(self, require_lock: bool) -> Snap:
        """Reads the screen; stops the run when someone else changed it or the key lock is gone."""
        now = self.snap()
        if not self._unchanged(now):
            raise InUse(f"{self.last} -> {now}")
        if require_lock and self.locked is False:
            raise InUse("the key lock ended (Esc held on the badge, or its timeout)")
        return now

    def send(self, keys: Sequence[str]) -> None:
        """A few keys, read before and after (the correction steps): Enter must open something."""
        if not keys:
            return
        now = self._check(False)
        reply = self.fw("keys send " + " ".join(keys))
        if not any(l.strip().startswith("keys sent") for l in reply):
            raise RouteError(f"keys send said: {' / '.join(reply)}")
        after = self.snap()
        self.last = after
        if "enter" in keys and len(after.stack) <= len(now.stack):
            raise RouteError(f"Enter opened nothing (stack {list(now.stack)} -> {list(after.stack)})")

    def correct(self, label: str, rows: Sequence[str]) -> None:
        """Single steps to row `label` of `rows` (one row took no focus, or a row was added), each read back."""
        target = rows.index(label)
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
        self._check(False)
        self.fw(line)
        self.last = self.snap()

    def open(self, app: str) -> None:
        """"fw ui open": from home, as from the app's tile. "Not now" (a dialog, a lock screen) stops the run."""
        reply = self.fw(f"ui open {app}")
        text = " / ".join(l.strip() for l in reply)
        if any(l.strip() == f"ui open: {app}" for l in reply):
            self.last = None  # a new screen: the guards in the next "keys send" check it
            return
        if "not now" in text:
            raise InUse(f"ui open {app}: {text}")
        raise RouteError(f"ui open {app}: {text or 'no reply'}")

    def home(self) -> None:
        self._check(False)
        self.open(HOME_APP)
        self.last = self.snap()

    def keys(self, tokens: Sequence[Token]) -> None:
        """One "keys send" for a run of keys and guards; a row guard that fails gets the single-step correction once."""
        corrected = set()
        todo = list(tokens)
        # No reading before each send: the walk read the screen just before (and the lock keeps the keyboard out); a
        # sequence the lock ended under is stopped by the badge itself
        while todo:
            guards = [t for t in todo if isinstance(t, Guard)]
            line = "keys send " + " ".join(t.token() if isinstance(t, Guard) else t for t in todo)
            reply = self.fw(line)
            self.last = None
            if any(l.strip().startswith("keys sent") for l in reply):
                return
            failed = next((GUARD_FAILED.match(l) for l in reply if GUARD_FAILED.match(l)), None)
            if any("key lock ended" in l for l in reply):
                raise InUse("the key lock ended during a sequence (Esc held on the badge, or its timeout)")
            if not failed:
                raise RouteError(f"keys send said: {' / '.join(reply)}")
            g = guards[int(failed.group(1)) - 1]
            if g.kind != "row" or g in corrected:
                raise RouteError(f"expected focus {'on' if g.kind == 'row' else 'containing'} {g.label!r} before "
                                 f"Enter, found {failed.group(2)!r}; nothing pressed")
            corrected.add(g)
            self.last = self.snap()
            self.correct(g.label, g.rows)
            todo = todo[todo.index(g):]  # again from the guard: the badge checks it once more before the Enter

    def walk(self, name: str, steps: Sequence[Step], expect_top: Optional[int] = None) -> None:
        lint_steps(name, steps)
        self._check(True)
        opened = bool(steps) and isinstance(steps[0], tuple) and steps[0][0] == "open"
        self.open(steps[0][1] if opened else HOME_APP)
        enters = sum(1 for st in steps if st == "enter")
        pending: List[Token] = []
        for st in steps[1:] if opened else steps:
            if isinstance(st, str):
                pending.append(st)
            elif st[0] == "row":
                pending += ["down"] * st[2].index(st[1]) + [Guard("row", st[1], tuple(st[2]))]
            elif st[0] == "focus":
                pending.append(Guard("focus", st[1]))
            else:  # any_row and fw need a reading first: send what is pending
                self.keys(pending)
                pending = []
                if st[0] == "any_row":
                    self.last = self.snap()
                    self.any_row()
                else:
                    self.console(st[1])
        self.keys(pending)
        self.last = self.snap()
        depth = len(self.last.stack)
        if depth < 1 + int(opened) + enters:
            raise RouteError(f"an Enter opened nothing (stack {list(self.last.stack)}, {enters} Enter(s))")
        top = self.last.stack[-1] if self.last.stack else None
        if expect_top is not None and top != expect_top:
            raise RouteError(f"route ended on screen {top}, expected {expect_top}")


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
    """Answers `fw ui` from a queue (the last one repeats), `keys send` from a queue (then "keys sent"), `ui open` and
    `ui lock` with fixed replies, and records every line."""

    def __init__(self, ui_replies: Sequence[Sequence[str]], keys: Sequence[str] = (), open_reply: Optional[str] = None,
                 lock_reply: str = "ui lock: on, 60 s after the last command (Esc held 3 s ends it)") -> None:
        self.ui = [list(r) for r in ui_replies]
        self.keys = list(keys)
        self.open_reply = open_reply
        self.lock_reply = lock_reply
        self.sent: List[str] = []
        self.lines: List[str] = []

    def __call__(self, line: str) -> List[str]:
        self.lines.append(line)
        if line.startswith("keys send"):
            self.sent.append(line)
            return [self.keys.pop(0) if self.keys else "keys sent: 1"]
        if line == "ui":
            return self.ui.pop(0) if len(self.ui) > 1 else list(self.ui[0])
        if line == "ui lock on":
            return [self.lock_reply]
        if line == "ui lock off":
            return ["ui lock: off"]
        if line.startswith("ui open "):
            return [self.open_reply if self.open_reply is not None else "ui open: " + line[len("ui open "):]]
        return []

    def opens(self) -> List[str]:
        return [l for l in self.lines if l.startswith("ui open ")]


def ui(stack: str, focus: str, locked: bool = True) -> List[str]:
    return [f"ui stack: {stack} | focus: {focus}", f"ui lock: {'on, 50 s left' if locked else 'off'}", "ui   x"]


def selftest_remote(expect: Callable[..., None], failures: List[str]) -> None:
    """#190: the key lock, "fw ui open" and one "keys send" per route with the guards inside it."""
    expect("lock refused", InUse, FakeFw([ui("0", "Settings")], lock_reply="unknown command 'ui'"),
           lambda b: b.lock(), 0)
    expect("lock ended before a route", InUse, FakeFw([ui("0", "Settings"), ui("0", "Settings", locked=False)]),
           lambda b: b.walk("weather", tile("Weather")), 0)
    fake = FakeFw([ui("0", "Settings"), ui("0", "Settings"), ui("0 > 7", "(none)")])
    expect("open, no keys", None, fake, lambda b: b.walk("weather", tile("Weather")), 0)
    if fake.opens() != ["ui open Weather"]:
        failures.append(f"open, no keys: opened {fake.opens()}")
    expect("open refused (dialog)", InUse, FakeFw([ui("0", "Settings")], open_reply="ui open: not now (a dialog is "
           "open)"), lambda b: b.walk("weather", tile("Weather")), 0)
    expect("open failed", RouteError, FakeFw([ui("0", "Settings")], open_reply="ui open: no app 'Weather'"),
           lambda b: b.walk("weather", tile("Weather")), 0)
    route = settings_page("Time") + [("row", "Time zone", TIME), "enter", "wait"]
    fake = FakeFw([ui("0", "Settings"), ui("0", "Settings"), ui("0 > 2 > 100 > 101", "Europe")])
    expect("one keys send per route", None, fake, lambda b: b.walk("tz", route), 1)
    want = "keys send " + "down " * 7 + "row:Time| enter wait down row:Time zone| enter wait"
    if fake.sent and fake.sent[0] != want:
        failures.append(f"one keys send per route: sent {fake.sent[0]!r}, wanted {want!r}")
    expect("focus guard fails", RouteError, FakeFw([ui("0", "MeshCore"), ui("0", "MeshCore")],
           keys=["keys send: guard 1 failed: focus 'Contacts'; 1 of 4 sent"]),
           lambda b: b.walk("chats", mc_grid(CHAT_TILE) + [("focus", "#badgetest"), "enter"]), 1)
    # The row guard finds "Time" (a row took no focus): one Up, read back, then the rest again from the guard
    fake = FakeFw([ui("0", "S"), ui("0", "S"), ui("0 > 2", "Time"), ui("0 > 2", "Time"), ui("0 > 2", "Display"),
                   ui("0 > 2 > 101", "Brightness")],
                  keys=["keys send: guard 1 failed: focus 'Time'; 7 of 10 sent"])
    expect("row guard corrected", None, fake, lambda b: b.walk("display", settings_page("Display")), 3)
    if len(fake.sent) == 3 and fake.sent[2] != "keys send row:Display| enter wait":
        failures.append(f"row guard corrected: resent {fake.sent[2]!r}")
    expect("row guard fails twice", RouteError, FakeFw(
        [ui("0", "S"), ui("0", "S"), ui("0 > 2", "Time"), ui("0 > 2", "Time"), ui("0 > 2", "Display"),
         ui("0 > 2", "Display")], keys=["keys send: guard 1 failed: focus 'Time'; 7 of 10 sent", "keys sent: 1",
                                        "keys send: guard 1 failed: focus 'Time'; 0 of 3 sent"]),
        lambda b: b.walk("display", settings_page("Display")), 3)
    expect("lock ended in a sequence", InUse, FakeFw([ui("0", "S")], keys=[
        "keys send: stopped, the key lock ended; 3 of 10 sent"]), lambda b: b.walk("display", settings_page("Display")), 1)
    expect("any_row reads first", None, FakeFw([ui("0", "S"), ui("0", "S"), ui("0 > 2 > 3", "Alice"),
                                                ui("0 > 2 > 3 > 4", "Message")]),
           lambda b: b.walk("contact", mc_grid(CONTACTS_TILE) + [("any_row",), "enter", "wait"]), 2)
    expect("any_row forbidden", RouteError, FakeFw([ui("0", "S"), ui("0", "S"), ui("0 > 2 > 3", "Remove")]),
           lambda b: b.walk("contact", mc_grid(CONTACTS_TILE) + [("any_row",), "enter", "wait"]), 1)
    expect("enter opened nothing", RouteError, FakeFw([ui("0", "S"), ui("0", "S"), ui("0 > 2", "Display")]),
           lambda b: b.walk("display", settings_page("Display")), 1)
    fake = FakeFw([ui("0", "S")], keys=["keys send: guard 1 failed: focus 'x'; 0 of 2 sent"])
    badge = Badge(fake)
    try:
        badge.start()
        badge.lock()
        badge.walk("chats", mc_grid(CHAT_TILE) + [("focus", "#badgetest"), "enter"])
    except RouteError:
        pass
    finally:
        release(badge)
    if fake.lines[-1] != "ui lock off":
        failures.append(f"unlock in finally: last line {fake.lines[-1]!r}")


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
    expect("stack changed between routes", InUse, FakeFw([ui("0", "Settings"), ui("0 > 3", "Wi-Fi")]),
           lambda b: b.walk("weather", tile("Weather")), 0)
    selftest_remote(expect, failures)
    for bad in ([("focus", "")], ["bksp"], [("focus", "Remove")], ["enter"], [("row", "Wi-Fi", ["Wi-Fi"]), "tab"],
                [("open", "Nope")], ["down", ("open", "Settings")], [("focus", "a|b"), "enter"]):
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
    return run(args)


def release(badge: Badge) -> None:
    """The key lock off, whatever happened: the person holding the badge gets the keys back at once."""
    try:
        badge.unlock()
    except RouteError as e:
        print(f"WARNING the key lock was not released ({e}); it ends by itself 60 s after the last command")


def run(args: argparse.Namespace) -> int:
    ip = args.ip
    ids = screen_id_map()
    badge = Badge(lambda line: run_badge_sh(ip, line))
    hits = errors = skipped = ran = 0
    kinds: Dict[str, int] = {k: 0 for k in KINDS}
    in_use = False
    try:
        badge.start()
        badge.lock()
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
                print("badge in use: stopped (not sent home)")
                in_use = True
        release(badge)
    by_kind = ", ".join(f"{k} {n}" for k, n in kinds.items())
    print(f"total: {hits} hit(s) ({by_kind}), {ran} screen(s) read, {errors} not read, {skipped} skipped")
    print(f"not reachable: {len(UNREACHABLE) + len(UNREACHABLE_APPS)} (python3 tools/ui_overflow_sweep.py --list)")
    return 2 if (errors or in_use) else (1 if hits else 0)


if __name__ == "__main__":
    sys.exit(main())
