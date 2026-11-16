#!/usr/bin/env python3
"""Use-soak driver (#476, #407): runs tools/soak_plan.py against the badge, like a user would.

  soak_driver.py --dry-run [--rehearsal]
  soak_driver.py --badge-ip-file FILE --run-dir DIR [--rehearsal] [--config FILE] [--issue N]

Every console line goes through `tools/badge.sh fw <address> <line>`, so the bench guard (tools/onair_guard.sh) judges
it; there is no other route, and the driver's own allowlist (soak_plan.line_allowed) refuses any line the plan cannot
produce. The address is read from the file named on the command line (one IPv4 address) and is never printed; what
goes into the transcript is redacted (the address, MACs, the configured scope names). The private config (default
~/.config/why2025-badge/soak-driver.conf, keys scope_zero_hop= and scope_regional=, names of at least 3 characters)
names the scopes; without it the driver refuses to start. Run directory: transcript.txt (the baseline soak's `## sample`
blocks, plus `## segment`, `## session` and `## state` marker lines), ledger.csv, state.txt, and on a stop BLOCKER. A
file named STOP in it ends the run cleanly; a STOP or a BLOCKER left from an earlier run ends or refuses the next one,
so remove them first. The ledger is ledger-<profile>.csv: a rerun in the same directory continues the send count from
it (the caps hold per run directory and profile, not per process; a `pending` row, written before a send goes out,
counts as an attempt; an unreadable row is a blocker), and a directory that holds the other profile's ledger (a
rehearsal, then the 24 h run) is refused: use a new --run-dir for each run.

Stop rule: after every state read the boot count, crash count, slot and C6 restart counter are compared with the start
values (a state read also follows the block, before the tail). A change, a badge that stops answering, a session that
does not end on home with the lock off, three failed or skipped sends in a row, or any unexpected error: BLOCKER gets
the reason (fixed text, never badge output), one comment goes to the issue (a failed comment is noted in BLOCKER), exit
1. After a change of the counters or no answer no further line is sent at all (the key lock runs out by itself after
LOCK_S seconds); after any other stop the session's tail (console scope back to the 0-hop scope, home, lock off) still
goes out. Ctrl-C and SIGTERM do the tail and exit 130 (the badge.sh call under way is killed). No self-repair.
Scope names: the console scope before the run is read once and kept in memory only (#478). At the end of the run
(finished, STOP, a non-silent blocker, Ctrl-C, SIGTERM) it is set back when the bench guard's allow file has it as a
`scope` line; otherwise (not allowed, or `not set`) the 0-hop scope stays and state.txt and the summary say so, without
a name. Every scope name a `mesh scope` reply shows is redacted too, in the echoed lines as well (#477).
Exit: 0 finished or STOP, 1 blocker, 2 bad usage or config, 130 interrupted."""
import argparse
import csv
import ipaddress
import os
import re
import signal
import subprocess
import sys
import time
from typing import Callable, Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_parse  # noqa: E402
import soak_plan as sp  # noqa: E402

DEFAULT_CONFIG = os.path.expanduser("~/.config/why2025-badge/soak-driver.conf")
SCOPE_RX = re.compile(r"^[a-z0-9][a-z0-9-]{2,}$")
SCOPE_LINE_RX = re.compile(r"^(?:fw: )?default [^ |]+ \| console ([^ |]+|not set)\s*$", re.M)
SCOPE_NAMES_RX = re.compile(r"^(?:fw: )?default ([^ |]+) \| console ([^ |]+|not set)\s*$", re.M)
STACK_RX = re.compile(r"^(?:fw: )?ui stack: (.*?) \|", re.M)
FOCUS_RX = re.compile(r"^(?:fw: )?ui stack: (.*?) \| focus: (.*?)\s*$", re.M)
GRID_STACK_RX = re.compile(r"^0 > \d+$")  # home, then the MeshCore app (its grid on top)
FAILED_RX = re.compile(r"not now|did not open|no app|guard \d+ failed|key lock is off|refused|not opened|"
                       r"not confirmed|dropped|developer mode is off|refusing: ", re.I)
MAC_RX = re.compile(r"([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
IP_RX = re.compile(r"(\d{1,3}\.){3}\d{1,3}")
STATE_KEYS = ("boot", "crash", "slot", "c6rs")
LEDGER_HEAD = ["seq", "time", "scope", "id", "tx_before", "tx_after", "result", "tx_air"]
# One tx row of `mesh airlog` (airlog_console_line, mesh_airlog.c): "#12 t=3405 tx grp_txt flood hops=0 len=61 retry=0
# result=ok". t is the badge uptime in seconds, the same clock as `up=` of `fw soak`. No address is printed.
AIRLOG_TX_RX = re.compile(r"^#(\d+) t=(\d+) tx (\S+) \S+ hops=\d+ len=(\d+) retry=\d+ result=(\w+)\s*$", re.M)
AIRLOG_SLACK_S = 1     # t= and up= are whole seconds, read at different moments
ATTEMPT_RESULTS = ("sent", "failed", "pending")  # a pending row: the line may have gone out, so it counts
RETRIES, RETRY_WAIT_S = 3, 20
SAMPLE_MISSES_MAX, SAMPLE_RETRY_S = 5, 60
BAD_MAX = 3            # failed or skipped sends in a row, or Contacts blocks that did not open, in a row
SETTLE_S = 3           # for the badge's tx counter to follow the send
REGIONAL_TOL_S = 900   # a reloaded regional attempt is compared on real time, with this much slack
CALL_TIMEOUT_S = 150


class Blocker(Exception):
    """A stop condition: the reason is fixed text, never badge output. silent: the badge restarted or does not
    answer, so no further line goes out (not even the tail)."""

    def __init__(self, reason: str, silent: bool = False):
        super().__init__(reason)
        self.silent = silent


class StepError(Exception):
    """One session line failed; the session skips to its tail."""


class Config:
    def __init__(self, zero_hop: str, regional: str):
        self.scopes = {sp.ZERO_HOP: zero_hop, sp.REGIONAL: regional}

    def names(self) -> Tuple[str, ...]:
        return tuple(self.scopes.values())


def norm_scope(name: str) -> str:
    name = name.strip().lower()
    return name[1:] if name.startswith("#") else name


def load_config(path: str) -> Config:
    """The private scope names. ValueError (without the values) when the file or a key is missing or malformed."""
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except OSError:
        raise ValueError("no config file %s: the driver will not start without its scope names" % path)
    kv: Dict[str, str] = {}
    for line in lines:
        key, eq, rest = line.partition("=")
        if eq and not key.lstrip().startswith("#"):
            m = re.fullmatch(r"\s*(\S+)(?:\s+#.*)?", rest)  # one word, then at most a comment ("#name" is a value)
            kv[key.strip()] = norm_scope(m.group(1)) if m else ""
    for key in ("scope_zero_hop", "scope_regional"):
        if not SCOPE_RX.match(kv.get(key, "")):
            raise ValueError("config %s: %s is missing or not a region name (3 or more of a-z, 0-9, '-')" % (path, key))
    if kv["scope_zero_hop"] == kv["scope_regional"]:
        raise ValueError("config %s: the two scopes must differ" % path)
    return Config(kv["scope_zero_hop"], kv["scope_regional"])


class RealClock:
    def now(self) -> float:
        return time.time()

    def mono(self) -> float:
        # CLOCK_BOOTTIME keeps counting while the host is suspended, so the lateness rule sees it (Linux)
        return time.clock_gettime(getattr(time, "CLOCK_BOOTTIME", time.CLOCK_MONOTONIC))

    def sleep(self, s: float) -> None:
        time.sleep(s)


class BadgeShell:
    """The only route to the badge: tools/badge.sh fw <address> <line>. The address stays in memory here."""

    def __init__(self, ip_file: str, repo: Optional[str] = None):
        self.repo = repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        with open(ip_file, encoding="utf-8") as f:
            text = f.read().strip()
        try:
            self.address = str(ipaddress.IPv4Address(text))
        except ValueError:
            raise ValueError("the address file must hold exactly one IPv4 address")

    def run(self, line: str) -> Tuple[int, str]:
        p = subprocess.Popen([os.path.join(self.repo, "tools", "badge.sh"), "fw", self.address, line],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                             start_new_session=True)
        try:
            out, _ = p.communicate(timeout=CALL_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            self.kill(p)  # badge.sh and the curl under it, so no child holds the pipe
            return 124, "timeout"
        except BaseException:  # Ctrl-C, SIGTERM: no call may go on unattended after the driver stops
            self.kill(p)
            raise
        return p.returncode, out

    @staticmethod
    def kill(p) -> None:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.communicate()


def install_signal_handlers() -> None:
    """SIGTERM ends the run like Ctrl-C: the tail goes out, the call under way is killed."""
    def stop(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)


def guard_allowed_scopes() -> List[str]:
    """The console scopes the bench guard allows (tools/onair_guard.sh onair_allowed_scopes), read through the guard's
    own function so the allow file is neither opened nor printed here. Empty when it cannot be asked."""
    guard = os.path.join(os.path.dirname(os.path.abspath(__file__)), "onair_guard.sh")
    try:
        p = subprocess.run(["bash", "-c", 'source "$1" && onair_allowed_scopes', "bash", guard], capture_output=True,
                           text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return []
    return p.stdout.split() if p.returncode == 0 else []


def repo_slug(repo: str) -> str:
    """owner/name of the repo's origin remote, '' when it cannot be told."""
    try:
        url = subprocess.run(["git", "-C", repo, "remote", "get-url", "origin"], capture_output=True, text=True,
                             timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""
    m = re.search(r"[:/]([^/:@]+/[^/]+?)(?:\.git)?$", url)
    return m.group(1) if m else ""


def post_issue_comment(issue: int, text: str) -> bool:
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    slug = repo_slug(repo)
    if not slug:
        return False
    try:
        p = subprocess.run(["gh", "issue", "comment", str(issue), "-R", slug, "--body", text], cwd=repo,
                           capture_output=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return False
    return p.returncode == 0


class Driver:
    def __init__(self, profile: sp.Profile, cfg: Config, badge, clock, run_dir: str,
                 notify: Callable[[str], Optional[bool]] = lambda text: True,
                 allowed_scopes: Optional[Callable[[], List[str]]] = None):
        self.profile, self.cfg, self.badge, self.clock, self.dir, self.notify = profile, cfg, badge, clock, run_dir, notify
        self.allowed_scopes = allowed_scopes or guard_allowed_scopes
        # Scope names the badge reported in a `mesh scope` reply, and the console scope before the run: in memory only,
        # never written anywhere (#477, #478). extra_names: the start scope once the bench guard allows it.
        self.seen_scopes: set = set()
        self.start_scope: Optional[str] = None
        self.extra_names: Tuple[str, ...] = ()
        self.start_note = ""
        self.start_wall = self.start_mono = 0.0
        self.run_id = ""
        self.base: Dict[str, int] = {}
        self.seq = 0
        # every send that reached `mesh send`: (monotonic time, scope kind, plan slot or None when reloaded)
        self.attempts: List[Tuple[float, str, Optional[int]]] = []
        self.bad = 0                # failed or skipped sends in a row
        self.contacts_bad = 0       # Contacts blocks in a row that could not open the list
        self.contacts_opened = False
        self.slot: Optional[int] = None
        self.scope_dirty = False    # the console scope may still be the regional one
        self.session_n = 0
        self.samples = 0
        self.last_raw = ""
        self.address = getattr(badge, "address", "")

    # ---- files ----------------------------------------------------------------------------------------------------
    def path(self, name: str) -> str:
        return os.path.join(self.dir, name)

    def scrub(self, text: str, learned: bool = False) -> str:
        """learned: also the names the badge reported. Only what is written uses it: a scope named like a command
        ('mem', 'soak') must not break the parsers, which read the configured-names-only text."""
        if self.address:
            text = text.replace(self.address, "<addr>")
        text = MAC_RX.sub("<mac>", IP_RX.sub("<ip>", text))
        for name in sorted(set(self.cfg.names()) | (self.seen_scopes if learned else set()), key=len, reverse=True):
            text = re.sub(r"(?<![\w-])#?" + re.escape(name) + r"(?![\w-])", "<scope>", text, flags=re.I)
        return text

    def learn_scopes(self, raw: str) -> None:
        """Remember every scope name a `mesh scope` reply shows so scrub() hides it (the badge's own scope is not a
        configured name). Every token counts, whatever its characters or length (the leak is the same); 'none' is the
        badge's word for no default and 'not set' for no scope, neither is a name."""
        for found in SCOPE_NAMES_RX.findall(raw.replace("\r", "")):
            for name in map(norm_scope, found):
                if name and name not in ("none", "not set"):
                    self.seen_scopes.add(name)

    def write(self, text: str) -> None:
        with open(self.path("transcript.txt"), "a", encoding="utf-8") as f:
            f.write(text.rstrip("\n") + "\n")

    def note(self, text: str) -> None:
        with open(self.path("state.txt"), "a", encoding="utf-8") as f:
            f.write("%d %s\n" % (self.clock.now(), text))

    def stop_requested(self) -> bool:
        return os.path.exists(self.path("STOP"))

    # ---- the one route to the badge --------------------------------------------------------------------------------
    def line(self, text: str, judged: Optional[bool] = None) -> str:
        """Send one console line, record it, return the redacted reply. StepError when the badge or the guard refused
        it (judged lines only: the reply of `mem` says 'refused 0' in a row). A line outside the allowlist never goes."""
        if not sp.line_allowed(text, self.cfg.names() + self.extra_names):
            raise Blocker("the plan produced a console line outside the driver's allowlist")
        rc, raw = self.badge.run(text)
        self.last_raw = raw  # unscrubbed, for the readers that need the scope name (scope_ready, the start scope)
        if text == "mesh scope":
            self.learn_scopes(raw)
        out = self.scrub(raw)  # what the parsers read
        self.write(">> " + self.scrub(text, True))
        self.write(self.scrub(raw, True))
        if rc != 0:
            raise StepError("'%s' failed (exit %d)" % (text.split()[0], rc))
        if (sp.is_checked(text) if judged is None else judged) and FAILED_RX.search(out):
            raise StepError("'%s' was refused or did not work" % " ".join(text.split()[:2]))
        return out

    # ---- state and the stop rule -----------------------------------------------------------------------------------
    @staticmethod
    def parse_state(reply: str) -> Optional[Dict[str, int]]:
        (s,) = soak_parse.usable(soak_parse.parse_reply(reply))[:1] or [None]
        if s is None:
            return None
        out = {}
        for key in STATE_KEYS + ("mtx", "up"):
            v = soak_parse.value(s, key)
            if v is not None:
                out[key] = int(v)
        return out if all(k in out for k in STATE_KEYS) else None

    def read_state(self, tries: int = RETRIES) -> Dict[str, int]:
        """`fw soak` parsed and compared with the start values; Blocker on a change or on no answer."""
        for i in range(tries):
            try:
                state = self.parse_state(self.badge_soak())
            except StepError:
                state = None
            if state is not None:
                self.check_state(state)
                return state
            if i + 1 < tries:
                self.clock.sleep(RETRY_WAIT_S)
        raise Blocker("the badge did not answer the state read", silent=True)

    def badge_soak(self) -> str:
        rc, out = self.badge.run("soak")
        if rc != 0:
            raise StepError("soak failed")
        return out

    def check_state(self, state: Dict[str, int]) -> None:
        if not self.base:
            self.base = {k: state[k] for k in STATE_KEYS}
            return
        names = {"boot": "the boot counter changed (a restart)", "crash": "the crash counter changed",
                 "slot": "the running slot changed", "c6rs": "the C6 restart counter changed"}
        for key in STATE_KEYS:
            if state[key] != self.base[key]:
                raise Blocker(names[key], silent=True)
        self.write("## state %s epoch=%d" % (" ".join("%s=%d" % (k, state[k]) for k in sorted(state) if k != "up"), self.clock.now()))

    @staticmethod
    def parse_tx_air(reply: str, up_from: Optional[int], up_to: Optional[int]) -> Tuple[str, str]:
        """(ledger value, transcript note) for this send: the badge's own channel-message tx entry closest at or after
        the send time (up_from), not after the read before it (up_to). `missing` when the airlog has none, or no time
        is known to match by."""
        if up_from is None or up_to is None:
            return "missing", "## tx_air missing (no uptime to match by)"
        hits = [(int(t), int(n), length, res) for n, t, kind, length, res in AIRLOG_TX_RX.findall(reply)
                if kind == "grp_txt" and up_from - AIRLOG_SLACK_S <= int(t) <= up_to + AIRLOG_SLACK_S]
        if not hits:
            return "missing", "## tx_air missing"
        t, _, length, res = min(hits)
        value = res if res in ("ok", "timeout") else "failed"
        return value, "## tx_air %s t=%d len=%s" % (value, t, length)

    def read_tx_air(self, up_from: Optional[int], up_to: Optional[int]) -> str:
        """One `mesh airlog` line through the guarded route; not a send, so no cap counts it. A badge that does not
        answer it gives `missing`, never a stop: the send itself already went."""
        try:
            reply = self.line("mesh airlog", judged=False)  # its header says "0 dropped": not a refusal
        except StepError:
            self.write("## tx_air missing (airlog read failed)")
            return "missing"
        value, note = self.parse_tx_air(reply, up_from, up_to)
        self.write(note)
        return value

    def check_home(self) -> None:
        rc, out = self.badge.run("ui")
        out = self.scrub(out)
        m = STACK_RX.search(out)
        if rc != 0 or m is None or m.group(1).strip() != "0" or "ui lock: off" not in out:
            raise Blocker("the session did not end on home with the key lock off")

    # ---- sends -----------------------------------------------------------------------------------------------------
    def ledger_path(self, profile: Optional[str] = None) -> str:
        return self.path("ledger-%s.csv" % (profile or self.profile.name))

    def ledger(self, row: List[object]) -> None:
        path = self.ledger_path()
        new = not os.path.exists(path)
        with open(path, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(LEDGER_HEAD)
            w.writerow(row)

    def ledger_update(self, row: List[object]) -> None:
        """Replace the row of this sequence number (the `pending` row of an attempt) by its outcome."""
        path = self.ledger_path()
        with open(path, newline="", encoding="utf-8") as f:
            rows = list(csv.reader(f))
        rows = [[str(x) for x in row] if r[:1] == [str(row[0])] else r for r in rows]
        tmp = path + ".tmp"
        with open(tmp, "w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerows(rows)
        os.replace(tmp, path)

    def load_ledger(self) -> None:
        """A rerun in the same run directory continues the count: every attempt (sent or failed) of the ledger counts
        toward the caps and the regional interval, at its own age."""
        path = self.ledger_path()
        if not os.path.exists(path):
            return
        now_wall, now_mono = self.clock.now(), self.clock.mono()
        try:
            with open(path, newline="", encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            for row in rows:
                seq, when, scope, result = int(row["seq"]), float(row["time"]), row["scope"], row["result"]
                if scope not in (sp.ZERO_HOP, sp.REGIONAL) or not (result in ATTEMPT_RESULTS or
                                                                   result.startswith("skipped: ")):
                    raise ValueError(result)
                self.seq = max(self.seq, seq)
                if result in ATTEMPT_RESULTS:
                    self.attempts.append((now_mono - max(0.0, now_wall - when), scope, None))
        except (ValueError, TypeError, KeyError, AttributeError, OSError, UnicodeError):
            raise Blocker("the ledger of an earlier run could not be read (a corrupt or cut row): the caps cannot be "
                          "trusted", silent=True)

    def cap_problem(self, kind: str, now: float, slot: Optional[int] = None) -> str:
        """now: monotonic. The regional interval is judged on the plan's slot times (jitter in when a send really
        happens must not skip it); a reloaded attempt has no slot and is judged on real time with some slack."""
        if len(self.attempts) >= self.profile.total_cap:
            return "total cap"
        if sum(1 for t, _, _ in self.attempts if now - t < sp.HOUR_S) >= self.profile.hour_cap:
            return "hour cap"
        if kind != sp.REGIONAL:
            return ""
        for t, k, at in self.attempts:
            if k != sp.REGIONAL:
                continue
            gap = slot - at if slot is not None and at is not None else now - t + REGIONAL_TOL_S
            if gap < self.profile.regional_every_s:
                return "regional interval"
        return ""

    def scope_ready(self, kind: str) -> bool:
        """The console scope read back from the badge is exactly one status line and equals the plan's; sets it once
        when it does not. A failed line counts as not ready."""
        want = self.cfg.scopes[kind]
        try:
            for attempt in range(2):
                self.line("mesh scope", judged=False)
                found = SCOPE_LINE_RX.findall(self.last_raw.replace("\r", ""))
                if len(found) == 1 and norm_scope(found[0]) == want:
                    return True
                if attempt == 0:
                    if kind == sp.REGIONAL:
                        self.scope_dirty = True
                    self.line("mesh scope console %s YES" % want, judged=False)
        except StepError:
            return False
        return False

    def restore_scope(self) -> None:
        """Back to the 0-hop scope after a regional attempt, so no later line (this driver's or the owner's) floods
        under the regional one. Best effort: a failure is noted and the next send checks the scope anyway."""
        if not self.scope_dirty:
            return
        if self.scope_ready(sp.ZERO_HOP):
            self.scope_dirty = False
        else:
            self.note("could not put the console scope back to the 0-hop scope")

    def read_start_scope(self) -> None:
        """The console scope before the run, kept in memory only; None for `not set` or a read that failed."""
        try:
            self.line("mesh scope", judged=False)
        except StepError:
            return
        found = SCOPE_LINE_RX.findall(self.last_raw.replace("\r", ""))
        name = norm_scope(found[0]) if len(found) == 1 else ""
        self.start_scope = name if SCOPE_RX.match(name) else None

    def end_restore(self) -> None:
        """At the end of the run: the console scope back to the start value when the bench guard allows it, else the
        0-hop scope stays, with a note (no name). Never raises, not even on a second Ctrl-C or SIGTERM: the caller's
        exit code and BLOCKER must survive it. Nothing is sent after a silent blocker (not called)."""
        try:
            self.restore_start_scope()
        except BaseException as e:  # noqa: BLE001
            self.start_note = "start scope not restored: error (%s)" % type(e).__name__
        if self.start_note:
            try:
                self.note(self.start_note)
            except BaseException:  # noqa: BLE001
                pass

    def restore_start_scope(self) -> None:
        self.restore_scope()  # the tail normally did this; a run that ended between sessions is already there
        want = self.start_scope
        if want is not None and want == self.cfg.scopes[sp.ZERO_HOP]:
            return
        # The regional scope floods: never the rest state, whatever the allow file says (owner default)
        if want is None or want == self.cfg.scopes[sp.REGIONAL] or \
                want not in [norm_scope(x) for x in self.allowed_scopes()]:
            self.start_note = "start scope not restored: not in the allow list"
            return
        self.extra_names = (want,)
        self.line("mesh scope console %s YES" % want, judged=False)
        self.line("mesh scope", judged=False)
        found = SCOPE_LINE_RX.findall(self.last_raw.replace("\r", ""))
        if not (len(found) == 1 and norm_scope(found[0]) == want):
            self.start_note = "start scope not restored: the badge did not take it"

    def send(self, kind: str) -> None:
        if kind == sp.REGIONAL:
            self.scope_dirty = True  # whatever happens next, the 0-hop scope goes back before this returns
        try:
            self.send_one(kind)
        except Blocker as b:
            if not b.silent:
                self.safe_restore()
            raise
        except BaseException:
            self.safe_restore()
            raise
        self.restore_scope()

    def safe_restore(self) -> None:
        """restore_scope on the way out of an exception: whatever it raises must not replace that exception."""
        try:
            self.restore_scope()
        except Exception as e:  # noqa: BLE001
            self.note("scope restore failed (%s)" % type(e).__name__)

    def skip(self, ident: str, kind: str, why: str) -> None:
        self.ledger([self.seq, int(self.clock.now()), kind, ident, "", "", "skipped: " + why])
        self.bad_send()

    def bad_send(self) -> None:
        self.bad += 1
        if self.bad >= BAD_MAX:
            raise Blocker("three sends failed or were skipped in a row")

    def send_one(self, kind: str) -> None:
        self.seq += 1
        text, ident = sp.send_line(self.run_id, self.seq)
        problem = self.cap_problem(kind, self.clock.mono(), self.slot)
        if not problem and not sp.target_is_test_channel(text):
            problem = "target"
        if not problem and not self.scope_ready(kind):
            problem = "scope read-back"
        if problem:
            return self.skip(ident, kind, problem)
        before = self.read_state()
        now = self.clock.now()
        self.attempts.append((self.clock.mono(), kind, self.slot))
        row = [self.seq, int(now), kind, ident, before.get("mtx", ""), "", "pending", ""]
        self.ledger(row)  # before the line goes out: a restart or Ctrl-C after it still leaves the attempt on record
        try:
            result = "sent" if "queued #" in self.line(text, judged=False) else "failed"
        except StepError:
            result = "failed"
        self.clock.sleep(SETTLE_S)
        after = self.read_state()
        tx_air = self.read_tx_air(before.get("up"), after.get("up")) if result == "sent" else ""
        self.ledger_update(row[:5] + [after.get("mtx", ""), result, tx_air])
        if result == "sent":
            self.bad = 0
        else:
            self.bad_send()

    # ---- sessions and samples --------------------------------------------------------------------------------------
    def contacts_gate(self) -> None:
        """Before the one Enter: the focus is exactly the Contacts tile and the stack is home > MeshCore (the grid on
        top). The firmware's own guard matches a substring, so a row like 'Contacts on the card' would pass it."""
        m = FOCUS_RX.search(self.scrub(self.line("ui", judged=False)))
        if m is None or GRID_STACK_RX.match(m.group(1).strip()) is None or m.group(2).strip() != "Contacts":
            raise StepError("the Contacts tile is not focused on the MeshCore grid: no Enter sent")

    def run_body(self, session: sp.Session) -> None:
        self.slot = session.start_s
        for step in sp.session_steps(session)[:-2]:
            if self.stop_requested():
                return
            if step.kind == "send":
                self.send(step.scope)
            elif step.text == sp.CONTACTS_ENTER:
                self.contacts_gate()
                self.line(step.text)
                self.contacts_opened = True
            else:
                self.line(step.text)
        self.read_state()  # a restart during the body ends the session here, before its tail

    def tail(self) -> str:
        """The lines that leave the badge on home with the lock off; every one is tried, whatever went wrong before."""
        error = ""
        try:
            self.restore_scope()
        except Exception as e:  # noqa: BLE001 - the tail must go on
            error = type(e).__name__
        for step in sp.home_steps():
            try:
                self.line(step.text)
            except (StepError, OSError, ValueError) as e:
                error = error or str(e) or type(e).__name__
        return error

    def session(self, s: sp.Session) -> None:
        self.write("## session %d %s start %d" % (s.n, s.block, self.clock.now()))
        error, pending = "", None
        self.contacts_opened = False
        try:
            self.run_body(s)
        except StepError as e:
            error = str(e)
        except BaseException as e:  # noqa: BLE001 - Blocker, KeyboardInterrupt, OSError: the tail first, then out
            pending = e
        if not (isinstance(pending, Blocker) and pending.silent):
            error = self.tail() or error
        if pending is not None:
            raise pending
        self.read_state()  # before check_home, so a restart is named as one and not as 'not on home'
        self.check_home()
        if error:
            self.write("## session %d %s error %s" % (s.n, s.block, error))
            self.note("session %d %s: %s" % (s.n, s.block, error))
        self.write("## session %d %s end %d" % (s.n, s.block, self.clock.now()))
        self.count_contacts(s)

    def count_contacts(self, s: sp.Session) -> None:
        """A Contacts block that never opened the list does nothing useful: three in a row stop the run, as sends do."""
        if s.block != "contacts" or self.stop_requested():
            return
        self.contacts_bad = 0 if self.contacts_opened else self.contacts_bad + 1
        if self.contacts_bad >= BAD_MAX:
            raise Blocker("the Contacts list could not be opened three times in a row")

    def sample(self) -> None:
        self.samples += 1
        self.write("## sample %d %s epoch=%d" % (self.samples, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(self.clock.now())),
                                                  self.clock.now()))
        misses = 0
        while True:
            try:
                reply = self.line("soak", judged=False)
            except StepError:
                reply = ""
            state = self.parse_state(reply)
            if state is not None:
                break
            misses += 1
            if misses >= SAMPLE_MISSES_MAX:
                raise Blocker("the badge did not answer for five retries", silent=True)
            self.clock.sleep(SAMPLE_RETRY_S)
        self.check_state(state)
        for cmd in ("mem", "tasks"):
            try:
                self.line(cmd, judged=False)
            except StepError:
                pass

    # ---- the run ---------------------------------------------------------------------------------------------------
    def timeline(self) -> List[Tuple[int, int, str, object]]:
        p = self.profile
        ev: List[Tuple[int, int, str, object]] = [(0, 0, "segment", "awake")]
        if p.night_s:
            ev.append((p.awake_s, 0, "segment", "night"))
        ev += [(t, 1, "sample", None) for t in range(0, p.duration_s + 1, p.sample_s)]
        ev += [(s.start_s, 2, "session", s) for s in sp.plan_sessions(p)]
        return sorted(ev, key=lambda e: (e[0], e[1]))

    def elapsed(self) -> float:
        return self.clock.mono() - self.start_mono

    def wait_until(self, t: float) -> bool:
        """Sleep to run offset t (monotonic) in short steps; False when STOP appeared."""
        while self.elapsed() < t:
            if self.stop_requested():
                return False
            self.clock.sleep(min(10, t - self.elapsed()))
        return not self.stop_requested()

    def run(self) -> int:
        os.makedirs(self.dir, mode=0o700, exist_ok=True)
        if os.path.exists(self.path("BLOCKER")):
            print("soak_driver: a BLOCKER file is in the run directory: look at it and remove it first", file=sys.stderr)
            return 2
        other = [n for n in os.listdir(self.dir) if re.fullmatch(r"ledger-.*\.csv", n) and n != os.path.basename(self.ledger_path())]
        if other:
            print("soak_driver: the run directory holds another profile's ledger: use a new --run-dir for each run",
                  file=sys.stderr)
            return 2
        try:
            self.read_state()  # the start values; a dead badge or developer mode off ends here
        except Blocker:
            print("soak_driver: the badge did not answer `soak` (developer mode off, or no route)", file=sys.stderr)
            return 2
        self.start_wall, self.start_mono = self.clock.now(), self.clock.mono()
        self.run_id = "%x" % int(self.start_wall)
        self.note("start: %s, run %s" % (self.profile.name, self.run_id))
        try:
            self.read_start_scope()
            self.load_ledger()
            self.note("earlier send attempts: %d" % len(self.attempts))
            rc = self.run_timeline()
        except Blocker as e:
            if not e.silent:
                self.end_restore()
            return self.block(str(e))
        except KeyboardInterrupt:
            self.note("interrupted")
            self.end_restore()
            return 130
        except Exception as e:  # noqa: BLE001 - the stop rule covers what nobody foresaw too
            self.end_restore()
            return self.block("unexpected error (%s)" % type(e).__name__)
        self.end_restore()
        return rc

    def run_timeline(self) -> int:
        for t, _, kind, payload in self.timeline():
            if not self.wait_until(t):
                self.note("stopped by the STOP file")
                return 0
            if kind == "session" and self.elapsed() - t > self.profile.every_s:
                self.write("## session %d %s skipped late" % (payload.n, payload.block))  # a suspended host: no catch-up
                continue
            self.dispatch(kind, payload)
        self.note("ended: %d samples, %d send attempts" % (self.samples, len(self.attempts)))
        return 0

    def dispatch(self, kind: str, payload) -> None:
        if kind == "segment":
            self.write("## segment %s %d" % (payload, self.clock.now()))
        elif kind == "sample":
            self.sample()
        else:
            self.session_n = payload.n
            self.session(payload)

    def block(self, reason: str) -> int:
        stamp = int(self.clock.now())
        if self.scope_dirty:
            reason += " (the console scope may still be the regional one: check `mesh scope`)"
        text = ("Use soak stopped by its driver at epoch %d, session %d: %s. The evidence is in the run directory "
                "on the bench host; nothing was changed on the badge." % (stamp, self.session_n, reason))
        try:
            posted = self.notify(text) is not False
        except Exception:  # noqa: BLE001
            posted = False
        with open(self.path("BLOCKER"), "a", encoding="utf-8") as f:
            f.write("%d %s%s\n" % (stamp, reason, "" if posted else " (the comment on the issue could not be posted)"))
        self.note("BLOCKER: " + reason)
        return 1


def dry_run(rehearsal: bool, out=sys.stdout) -> int:
    p = sp.profile_by_name(rehearsal)
    sessions = sp.plan_sessions(p)
    print("soak plan %s: awake %d min, night %d min, a session every %d min, %d sends a session, sampler every %d min" % (
        p.name, p.awake_s // 60, p.night_s // 60, p.every_s // 60, p.sends_per_session, p.sample_s // 60), file=out)
    for s in sessions:
        print("session %2d  t+%02d:%02d  %-8s %-9s sends: %s" % (s.n, s.start_s // 3600, s.start_s % 3600 // 60, s.segment,
                                                              s.block, ", ".join(s.sends) or "-"), file=out)
    if p.night_s:
        print("night      t+%02d:00  no sessions until t+%02d:00" % (p.awake_s // 3600, p.duration_s // 3600), file=out)
    t = sp.totals(p)
    print("totals: %d sessions (%d in the night), per block %s" % (
        t["sessions"], t["night_sessions"], ", ".join("%s %d" % kv for kv in t["per_block"].items())), file=out)
    print("sends: %d (%d zero_hop, %d regional), at most %d in any hour, caps %d an hour and %d in all, to %s only" % (
        t["sends"], t["zero_hop"], t["regional"], t["max_per_hour"], p.hour_cap, p.total_cap, sp.TEST_CHANNEL), file=out)
    return 0


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="no badge: print the schedule, the sends and the totals")
    ap.add_argument("--rehearsal", action="store_true", help="the whole block rotation once in one hour, at most 6 sends")
    ap.add_argument("--badge-ip-file", help="file holding the badge address (one IPv4 address; never printed)")
    ap.add_argument("--run-dir", help="directory for the transcript, ledger, state, BLOCKER and STOP")
    ap.add_argument("--config", default=DEFAULT_CONFIG, help="private scope names (default %(default)s)")
    ap.add_argument("--issue", type=int, default=407, help="issue that gets the stop comment")
    a = ap.parse_args(argv[1:])
    if a.dry_run:
        return dry_run(a.rehearsal)
    if not a.badge_ip_file or not a.run_dir:
        print("soak_driver: --badge-ip-file and --run-dir are required (or --dry-run)", file=sys.stderr)
        return 2
    try:
        cfg = load_config(a.config)
        badge = BadgeShell(a.badge_ip_file)
    except (ValueError, OSError) as e:
        print("soak_driver: %s" % (e if isinstance(e, ValueError) else "cannot read the address file"), file=sys.stderr)
        return 2
    install_signal_handlers()
    d = Driver(sp.profile_by_name(a.rehearsal), cfg, badge, RealClock(), a.run_dir,
               lambda text: post_issue_comment(a.issue, text))
    rc = d.run()
    if d.start_note:
        print("soak_driver: " + d.start_note)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
