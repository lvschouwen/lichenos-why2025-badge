#!/usr/bin/env python3
"""Use-soak driver (#476, #407): runs tools/soak_plan.py against the badge, like a user would.

  soak_driver.py --dry-run [--rehearsal]
  soak_driver.py --badge-ip-file FILE --run-dir DIR [--rehearsal] [--config FILE] [--issue N]

Every console line goes through `tools/badge.sh fw <address> <line>`, so the bench guard (tools/onair_guard.sh) judges
it; there is no other route. The address is read from the file named on the command line and is never printed;
what goes into the transcript is redacted (addresses, MACs, the configured scope names). The private config
(default ~/.config/why2025-badge/soak-driver.conf, keys scope_zero_hop= and scope_regional=) names the scopes; without
it the driver refuses to start. Run directory: transcript.txt (the baseline soak's `## sample` blocks, plus
`## segment`, `## session` and `## state` marker lines), ledger.csv, state.txt, and on a stop BLOCKER. A file named STOP
in it ends the run cleanly.

Stop rule: after every state read the boot count, crash count, slot and C6 restart counter are compared with the start
values. A change, a badge that stops answering, a session that does not end on home with the lock off, or three failed
send checks in a row: no further line is sent (not even the session's tail; the key lock runs out by itself after
LOCK_S seconds), BLOCKER gets the reason, one fixed-text comment goes to the issue, exit 1. No self-repair.
Exit: 0 finished or STOP, 1 blocker, 2 bad usage or config."""
import argparse
import csv
import os
import re
import subprocess
import sys
import time
from typing import Callable, Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_parse  # noqa: E402
import soak_plan as sp  # noqa: E402

DEFAULT_CONFIG = os.path.expanduser("~/.config/why2025-badge/soak-driver.conf")
SCOPE_RX = re.compile(r"^[a-z0-9][a-z0-9-]*$")
SCOPE_LINE_RX = re.compile(r"^(?:fw: )?default [^ |]+ \| console ([^ |]+|not set)\s*$", re.M)
STACK_RX = re.compile(r"^(?:fw: )?ui stack: (.*?) \|", re.M)
FAILED_RX = re.compile(r"not now|did not open|no app|guard \d+ failed|key lock is off|refused|not opened|"
                       r"not confirmed|dropped|developer mode is off|refusing: ", re.I)
IP_RX = re.compile(r"(\d{1,3}\.){3}\d{1,3}")
MAC_RX = re.compile(r"([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}")
STATE_KEYS = (("boot", "boot"), ("crash", "crash"), ("slot", "slot"), ("c6rs", "c6rs"))
RETRIES, RETRY_WAIT_S = 3, 20
SAMPLE_MISSES_MAX, SAMPLE_RETRY_S = 5, 60
SKIPS_MAX = 3
SETTLE_S = 3  # for the badge's tx counter to follow the send


class Blocker(Exception):
    """A stop condition: the reason is fixed text, never badge output."""


class StepError(Exception):
    """One session line failed; the session skips to its tail."""


class Config:
    def __init__(self, zero_hop: str, regional: str):
        self.scopes = {sp.ZERO_HOP: zero_hop, sp.REGIONAL: regional}


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
        line = re.sub(r"(^|\s)#.*$", "", line).strip()  # a comment starts at "#" after a space ("#name" is a value)
        key, eq, val = line.partition("=")
        if eq:
            kv[key.strip()] = norm_scope(val)
    for key in ("scope_zero_hop", "scope_regional"):
        if not SCOPE_RX.match(kv.get(key, "")):
            raise ValueError("config %s: %s is missing or not a region name" % (path, key))
    if kv["scope_zero_hop"] == kv["scope_regional"]:
        raise ValueError("config %s: the two scopes must differ" % path)
    return Config(kv["scope_zero_hop"], kv["scope_regional"])


class RealClock:
    def now(self) -> float:
        return time.time()

    def sleep(self, s: float) -> None:
        time.sleep(s)


class BadgeShell:
    """The only route to the badge: tools/badge.sh fw <address> <line>. The address stays in memory here."""

    def __init__(self, ip_file: str, repo: Optional[str] = None):
        self.repo = repo or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        with open(ip_file, encoding="utf-8") as f:
            self.address = f.read().strip()

    def run(self, line: str) -> Tuple[int, str]:
        try:
            p = subprocess.run([os.path.join(self.repo, "tools", "badge.sh"), "fw", self.address, line],
                               capture_output=True, text=True, timeout=150, errors="replace")
        except subprocess.TimeoutExpired:
            return 124, "timeout"
        return p.returncode, p.stdout + p.stderr


def post_issue_comment(issue: int, text: str) -> None:
    subprocess.run(["gh", "issue", "comment", str(issue), "--body", text], capture_output=True, timeout=60)


class Driver:
    def __init__(self, profile: sp.Profile, cfg: Config, badge, clock, run_dir: str,
                 notify: Callable[[str], None] = lambda text: None):
        self.profile, self.cfg, self.badge, self.clock, self.dir, self.notify = profile, cfg, badge, clock, run_dir, notify
        self.start: Optional[float] = None
        self.run_id = ""
        self.base: Dict[str, int] = {}
        self.seq = 0
        self.attempts: List[Tuple[float, str]] = []  # (time, scope kind) of every send that reached `mesh send`
        self.skips = 0
        self.session_n = 0
        self.samples = 0
        self.last_raw = ""
        self.redact = [cfg.scopes[sp.ZERO_HOP], cfg.scopes[sp.REGIONAL]]

    # ---- files ----------------------------------------------------------------------------------------------------
    def path(self, name: str) -> str:
        return os.path.join(self.dir, name)

    def scrub(self, text: str) -> str:
        text = MAC_RX.sub("<mac>", IP_RX.sub("<ip>", text))
        for name in self.redact:
            text = re.sub(r"#?" + re.escape(name), "<scope>", text, flags=re.I)
        return text

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
        it (judged lines only: the reply of `mem` says 'refused 0' in a row)."""
        rc, raw = self.badge.run(text)
        self.last_raw = raw  # unscrubbed, for the one reader that needs the scope name (scope_ready)
        out = self.scrub(raw)
        self.write(">> " + text)
        self.write(out)
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
        for key, name in STATE_KEYS + (("mtx", "mtx"),):
            v = soak_parse.value(s, name)
            if v is not None:
                out[key] = int(v)
        return out if all(k in out for k, _ in STATE_KEYS) else None

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
        raise Blocker("the badge did not answer the state read")

    def badge_soak(self) -> str:
        rc, out = self.badge.run("soak")
        if rc != 0:
            raise StepError("soak failed")
        return out

    def check_state(self, state: Dict[str, int]) -> None:
        if not self.base:
            self.base = {k: state[k] for k, _ in STATE_KEYS}
            return
        names = {"boot": "the boot counter changed (a restart)", "crash": "the crash counter changed",
                 "slot": "the running slot changed", "c6rs": "the C6 restart counter changed"}
        for key, _ in STATE_KEYS:
            if state[key] != self.base[key]:
                raise Blocker(names[key])
        self.write("## state %s epoch=%d" % (" ".join("%s=%d" % (k, state[k]) for k in sorted(state)), self.clock.now()))

    def check_home(self) -> None:
        rc, out = self.badge.run("ui")
        out = self.scrub(out)
        m = STACK_RX.search(out)
        if rc != 0 or m is None or m.group(1).strip() != "0" or "ui lock: off" not in out:
            raise Blocker("the session did not end on home with the key lock off")

    # ---- sends -----------------------------------------------------------------------------------------------------
    def ledger(self, row: List[object]) -> None:
        path = self.path("ledger.csv")
        new = not os.path.exists(path)
        with open(path, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new:
                w.writerow(["seq", "time", "scope", "id", "tx_before", "tx_after", "result"])
            w.writerow(row)

    def cap_problem(self, kind: str, now: float) -> str:
        if len(self.attempts) >= self.profile.total_cap:
            return "total cap"
        if sum(1 for t, _ in self.attempts if now - t < sp.HOUR_S) >= self.profile.hour_cap:
            return "hour cap"
        last = [t for t, k in self.attempts if k == sp.REGIONAL]
        if kind == sp.REGIONAL and last and now - last[-1] < self.profile.regional_every_s:
            return "regional interval"
        return ""

    def scope_ready(self, kind: str) -> bool:
        """The console scope read back from the badge equals the plan's; sets it once when it does not."""
        want = self.cfg.scopes[kind]
        for attempt in range(2):
            self.line("mesh scope", judged=False)
            m = SCOPE_LINE_RX.search(self.last_raw.replace("\r", ""))
            if m and norm_scope(m.group(1)) == want:
                return True
            if attempt == 0:
                self.line("mesh scope console %s YES" % want, judged=False)
        return False

    def send(self, kind: str) -> None:
        self.seq += 1
        text, ident = sp.send_line(self.run_id, self.seq)
        now = self.clock.now()
        problem = self.cap_problem(kind, now)
        if not problem and not sp.target_is_test_channel(text):
            problem = "target"
        if not problem and not self.scope_ready(kind):
            problem = "scope read-back"
        if problem:
            self.skips += 1
            self.ledger([self.seq, int(now), kind, ident, "", "", "skipped: " + problem])
            if self.skips >= SKIPS_MAX:
                raise Blocker("three send checks failed in a row")
            return
        self.skips = 0
        before = self.read_state()
        self.attempts.append((now, kind))
        try:
            reply = self.line(text, judged=False)
            result = "sent" if "queued #" in reply else "failed"
        except StepError:
            result = "failed"
        self.clock.sleep(SETTLE_S)
        after = self.read_state()
        self.ledger([self.seq, int(now), kind, ident, before.get("mtx", ""), after.get("mtx", ""), result])

    # ---- sessions and samples --------------------------------------------------------------------------------------
    def run_body(self, session: sp.Session) -> None:
        for step in sp.session_steps(session)[:-2]:
            if self.stop_requested():
                return
            if step.kind == "send":
                self.send(step.scope)
            else:
                self.line(step.text)

    def session(self, s: sp.Session) -> None:
        self.write("## session %d %s start %d" % (s.n, s.block, self.clock.now()))
        error = ""
        try:
            self.run_body(s)
        except Blocker:
            raise
        except StepError as e:
            error = str(e)
        for step in sp.home_steps():  # also after an error: the badge goes home and the keys come back
            try:
                self.line(step.text)
            except StepError as e:
                error = error or str(e)
        self.check_home()
        self.read_state()
        if error:
            self.write("## session %d %s error %s" % (s.n, s.block, error))
            self.note("session %d %s: %s" % (s.n, s.block, error))
        self.write("## session %d %s end %d" % (s.n, s.block, self.clock.now()))

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
                raise Blocker("the badge did not answer for five retries")
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

    def wait_until(self, t: float) -> bool:
        """Sleep to run offset t in short steps; False when STOP appeared."""
        while self.clock.now() - self.start < t:
            if self.stop_requested():
                return False
            self.clock.sleep(min(10, t - (self.clock.now() - self.start)))
        return not self.stop_requested()

    def run(self) -> int:
        os.makedirs(self.dir, mode=0o700, exist_ok=True)
        if os.path.exists(self.path("BLOCKER")):
            print("soak_driver: a BLOCKER file is in the run directory: look at it and remove it first", file=sys.stderr)
            return 2
        try:
            self.read_state()  # the start values; a dead badge or developer mode off ends here
        except Blocker:
            print("soak_driver: the badge did not answer `soak` (developer mode off, or no route)", file=sys.stderr)
            return 2
        self.start = self.clock.now()
        self.run_id = "%04x" % (int(self.start) & 0xFFFF)
        self.note("start: %s, run %s" % (self.profile.name, self.run_id))
        try:
            for t, _, kind, payload in self.timeline():
                if not self.wait_until(t):
                    self.note("stopped by the STOP file")
                    return 0
                self.dispatch(kind, payload)
        except Blocker as e:
            return self.block(str(e))
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
        with open(self.path("BLOCKER"), "a", encoding="utf-8") as f:
            f.write("%d %s\n" % (stamp, reason))
        self.note("BLOCKER: " + reason)
        self.notify("Use soak stopped by its driver at epoch %d, session %d: %s. The evidence is in the run directory "
                    "on the bench host; nothing was changed on the badge." % (stamp, self.session_n, reason))
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
    ap.add_argument("--badge-ip-file", help="file holding the badge address (never printed)")
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
    d = Driver(sp.profile_by_name(a.rehearsal), cfg, badge, RealClock(), a.run_dir,
               lambda text: post_issue_comment(a.issue, text))
    return d.run()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
