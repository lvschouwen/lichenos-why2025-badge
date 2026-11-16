"""Use-soak plan (#476, #407): the day profile, the block rotation, the send budget and the console lines of each
block. Pure logic: no I/O, no clock, no badge. tools/soak_driver.py runs it; the host test pins it.

A day is `awake_s` seconds with a session every `every_s`, then `night_s` seconds with none. A session is one batched
console run with the keyboard locked: `idle test 0`, `ui lock on`, the block's lines, then ALWAYS `ui open home` and
`ui lock off` (the driver runs those two also after an error). Blocks rotate chat, contacts, tour, files, web, idle.

What it cannot emulate (said here so nobody reads a result as more than it is):
- One Enter in the whole plan: the contacts block opens MeshCore's Contacts list with `keys send needlock
  focus:Contacts| enter`, which the firmware stops when the focused tile is not Contacts (ui_keys.c guards; the tile's
  name is its first label, mesh_grid.c NAMES). Inside the list it only scrolls, never opens a contact. Messages are sent
  from the console, not typed into the compose box (a remote Enter is refused on send screens and careful dialogs, by
  design); the compose screen, dialogs and real key presses stay hands checks.
- The Files block only browses the storage root (a copy needs a key the plan does not allow unattended).
- The keyboard is locked about a minute in twenty; a key pressed during a session is ignored.
- Inbound traffic and direct messages come from the meshcore peer session, which the owner starts.

Sends: console lines `mesh send #badgetest ...` only; the first send of the session at each regional interval goes
under the regional scope, every other under the 0-hop scope. Scope NAMES are never here: a send carries the kind
(ZERO_HOP / REGIONAL) and the driver maps it through its private config."""
import re
from dataclasses import dataclass
from typing import List, Optional, Tuple

TEST_CHANNEL = "#badgetest"
BLOCKS = ("chat", "contacts", "tour", "files", "web", "idle")
ZERO_HOP, REGIONAL = "zero_hop", "regional"
LOCK_S = 120          # ui lock on <s>: s seconds after the last command, so a dead driver frees the keys
SEND_SPACING_S = 20   # nominal seconds between the sends of one session, for the hourly check of the plan
HOUR_S = 3600

# Lines whose reply says whether they worked (a refused open, a stopped key sequence, a failed guard); the driver
# judges these. `mem`, `ui perf` and the like are read only and not judged (mem prints "refused 0" in its rows).
CHECKED_PREFIXES = ("keys send", "ui open", "ui lock on", "mesh chat")
CONTACTS_ENTER = "keys send needlock focus:Contacts| enter"  # the only Enter of the plan
APP_IDS = ("home", "meshcore", "clock", "timer", "calc", "weather", "notes", "qrcode", "lora_spectrum", "settings", "files")
KEY_WORDS = ("up", "down", "right", "wait")


@dataclass(frozen=True)
class Profile:
    name: str
    awake_s: int
    night_s: int
    every_s: int
    sends_per_session: int
    regional_every_s: int
    total_cap: int     # most sends in the whole run
    hour_cap: int      # most sends in any 3600 s
    sample_s: int      # sampler period

    @property
    def duration_s(self) -> int:
        return self.awake_s + self.night_s


FULL = Profile("24h", awake_s=16 * 3600, night_s=8 * 3600, every_s=1200, sends_per_session=2,
               regional_every_s=4 * 3600, total_cap=100, hour_cap=6, sample_s=600)
# One hour, the whole rotation once (a session every 10 minutes), at most 6 sends
REHEARSAL = Profile("rehearsal", awake_s=3600, night_s=0, every_s=600, sends_per_session=1,
                    regional_every_s=4 * 3600, total_cap=6, hour_cap=6, sample_s=300)


@dataclass(frozen=True)
class Session:
    n: int                      # 1-based
    block: str
    start_s: int                # seconds from the start of the run
    segment: str                # "awake" (sessions exist only there)
    sends: Tuple[str, ...]      # scope kinds, in the order they go out


@dataclass(frozen=True)
class Step:
    kind: str       # "line" or "send"
    text: str = ""  # the console line (without "fw "); empty for a send, the driver builds it
    scope: str = ""  # a send's scope kind


def segment_at(profile: Profile, t_s: float) -> str:
    return "awake" if t_s < profile.awake_s else "night"


def plan_sessions(profile: Profile) -> List[Session]:
    """Every session of the run, in time order. No session starts in the night."""
    out: List[Session] = []
    for k in range(profile.awake_s // profile.every_s):
        start = k * profile.every_s
        regional_due = start % profile.regional_every_s == 0
        sends = tuple(REGIONAL if (regional_due and i == 0) else ZERO_HOP for i in range(profile.sends_per_session))
        out.append(Session(k + 1, BLOCKS[k % len(BLOCKS)], start, "awake", sends))
    return out


def send_schedule(sessions: List[Session]) -> List[Tuple[int, str]]:
    """(nominal second, scope kind) of every send of the plan."""
    return [(s.start_s + SEND_SPACING_S * (i + 1), kind) for s in sessions for i, kind in enumerate(s.sends)]


def max_in_window(times: List[int], window_s: int = HOUR_S) -> int:
    """The most sends in any half-open window of window_s seconds."""
    ts = sorted(times)
    best, lo = 0, 0
    for hi, t in enumerate(ts):
        while t - ts[lo] >= window_s:
            lo += 1
        best = max(best, hi - lo + 1)
    return best


def keys(*names: str) -> str:
    """One `keys send` line; needlock first, so it stops when the keyboard lock is not on."""
    return "keys send needlock " + " ".join(names)


def scroll(n: int) -> List[str]:
    """Down n rows and back, in lines of at most 20 keys (a sequence takes 128 keys and about 0.25 s each)."""
    out = [keys(*["down"] * min(20, n - i)) for i in range(0, n, 20)]
    return out + [keys(*["up"] * min(20, n - i)) for i in range(0, n, 20)]


def block_lines(block: str) -> Tuple[List[str], List[str]]:
    """(before the sends, after the sends) for a block, without the session's head and tail. Sources, all in
    firmware/: `ui open <id|home>` ui_remote.c ui_remote_open_cmd; `keys send` lichen_shell/shell.c cmd_keys with the
    names of ui_keys.c; `mesh chat open|close` mesh/mesh_cmd_send.cpp chat_open/chat_close; `mem` fw_console/console.c;
    `ui perf` lichen_shell/ui_probe.c cmd_ui; `c6` c6link/c6link.c."""
    if block == "chat":
        return (["mesh chat open " + TEST_CHANNEL],
                [keys("up", "up", "up", "down", "down", "down"), "mesh chat close"])
    if block == "contacts":
        # ui open meshcore (MESHCORE_APP_ID), right to the second tile (mesh_grid.c IDS: chat, contacts), then the one
        # guarded Enter; the list is only scrolled
        return (["ui open meshcore", keys("right"), CONTACTS_ENTER, keys("wait")] + scroll(60) + ["ui perf"], [])
    if block == "tour":
        apps = ("clock", "timer", "calc", "weather", "notes", "qrcode", "lora_spectrum", "settings")
        lines = [x for a in apps for x in ("ui open " + a, keys("wait"))]
        return (lines + ["mem"], [])
    if block == "files":
        return (["ui open files", keys("wait")] + scroll(12) + ["ui perf"], [])
    if block == "web":
        return (["c6", "mem", "ui open weather", keys("wait", "wait")], [])
    return ([], [])  # idle: the session wakes the badge and leaves it again


def session_steps(session: Session) -> List[Step]:
    """The steps of one session, in order. The last two lines leave the badge on home with the lock off."""
    before, after = block_lines(session.block)
    steps = [Step("line", "idle test 0"), Step("line", "ui lock on %d" % LOCK_S)]
    steps += [Step("line", t) for t in before]
    steps += [Step("send", scope=kind) for kind in session.sends]
    steps += [Step("line", t) for t in after]
    return steps + home_steps()


def home_steps() -> List[Step]:
    return [Step("line", "ui open home"), Step("line", "ui lock off")]


def send_line(run_id: str, seq: int) -> Tuple[str, str]:
    """-> (console line, its id text). The id text is what the delivery ledger and the peer's log are matched on."""
    ident = "soak %s %d" % (run_id, seq)
    return "mesh send %s %s" % (TEST_CHANNEL, ident), ident


def target_is_test_channel(line: str) -> bool:
    """The driver's own check, independent of the bench guard: exactly `mesh send #badgetest <text>`, nothing else."""
    words = line.split()
    return len(words) >= 4 and words[0] == "mesh" and words[1] == "send" and words[2] == TEST_CHANNEL


def line_allowed(line: str, scope_names: Tuple[str, ...]) -> bool:
    """The driver's allowlist, a second guard behind the plan: only the console lines the plan can produce. No Enter
    except CONTACTS_ENTER, no dm, advert, remote, trace, discover, telemetry, history; `mesh scope console` only with
    one of the two configured names."""
    if not line or line != " ".join(line.split()) or any(ord(c) < 32 or ord(c) == 127 for c in line):
        return False  # one line, single spaces, no control character
    if line in ("idle test 0", "ui lock off", "ui", "ui perf", "soak", "mem", "tasks", "c6", "mesh scope",
                "mesh chat close", "mesh airlog", "mesh chat open " + TEST_CHANNEL, CONTACTS_ENTER):
        return True
    w = line.split()
    if len(w) == 4 and w[:3] == ["ui", "lock", "on"] and w[3].isdigit() and 1 <= int(w[3]) <= 600:
        return True
    if len(w) == 3 and w[:2] == ["ui", "open"] and w[2] in APP_IDS:
        return True
    if len(w) >= 4 and w[:3] == ["keys", "send", "needlock"]:
        return all(x in KEY_WORDS for x in w[3:])
    if len(w) == 5 and w[:3] == ["mesh", "scope", "console"] and w[4] == "YES":
        return w[3] in scope_names
    return len(w) == 6 and target_is_test_channel(line) and w[3] == "soak" and w[5].isdigit() and \
        re.fullmatch(r"[0-9a-f]+", w[4]) is not None


def is_checked(line: str) -> bool:
    return line.startswith(CHECKED_PREFIXES)


def profile_by_name(rehearsal: bool) -> Profile:
    return REHEARSAL if rehearsal else FULL


def totals(profile: Profile) -> dict:
    """The numbers --dry-run prints."""
    sessions = plan_sessions(profile)
    sends = send_schedule(sessions)
    regional = [t for t, k in sends if k == REGIONAL]
    per_block = {b: sum(1 for s in sessions if s.block == b) for b in BLOCKS}
    return {"sessions": len(sessions), "per_block": per_block, "sends": len(sends),
            "zero_hop": len(sends) - len(regional), "regional": len(regional),
            "max_per_hour": max_in_window([t for t, _ in sends]) if sends else 0,
            "awake_s": profile.awake_s, "night_s": profile.night_s,
            "night_sessions": sum(1 for s in sessions if s.start_s >= profile.awake_s)}


def first_session_after(sessions: List[Session], t_s: float) -> Optional[Session]:
    return next((s for s in sessions if s.start_s >= t_s), None)
