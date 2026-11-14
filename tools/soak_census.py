"""Log census of a serial log (#430, #407 check A), used by `soak_report.py census <serial-log>`.
Every line becomes a KIND: level, tag and the text with its numbers taken out. Kinds are counted per window (default
2 h) after a warm-up (default 2 h). Flagged, report only (no pass limit): a kind that first appears after the warm-up;
a kind whose rate rises (last window against the first after the warm-up); and, for every number inside a kind
("took # ms"), a median or maximum that rises between the first window after the warm-up and the last.
Timestamps: the flash host's logger writes `HH:MM:SS ` (tools/serial_log.py, local clock, midnight wraps, a small
step back is held); a line's ESP-IDF `I (12345) tag:` stamp is milliseconds since boot, and a stamp that drops is a
reboot: a new boot segment, reported. Without wall stamps the boot-relative stamps are chained across segments.
The log is read as a stream; per kind and window only a bounded sample of numbers is kept.
SCRUBBING IS FAIL-CLOSED: a kind is printed only after quoted strings, anything after a free-text marker (`name=`,
`msg:` ...), the word after `from`, `named`, `channel` ..., addresses (MAC, IPv4, IPv6), hex strings, long tokens,
paths and every number have been replaced. A kind that still holds a digit, quote or `@` is withheld. Free text in a
position no marker names cannot be recognised: read the output before it leaves the machine. Standard library only."""
import os
import random
import re
import statistics
import sys
from typing import Dict, Iterable, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_trend  # noqa: E402

M = "\uE000-\uF8FF"           # a number is one private-use char plus fillers (one per further digit) while a line is scrubbed
FILL = "\uF8FF"
MARK = "\uE000-\uF8FE"
DIGITS = re.compile(r"\d+")
DIGIT = re.compile(r"\d")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LINE = re.compile(r"^(?:(\d\d):(\d\d):(\d\d) )?(?:([EWIDV]) \((\d+)\) ([^\s:]+): ?)?(.*)$")
MAX_NUMBERS = 6000
MAX_KINDS = 20000
CACHE_MAX = 200000
SAMPLE_CAP = 512
MIN_VALUES = 3
RISE = 1.25
NUMBERED = re.compile("[%s]" % M)
HEXSET = "0-9a-fA-F" + M
_STEPS = [
    (re.compile(r'"[^"]*(?:"|$)'), "<str>"),
    (re.compile(r"'[^']*(?:'|$)"), "<str>"),
    (re.compile(r"(?i)\b((?:name|ssid|node|contact|nick|sender|user|msg|message|text|payload|body|title|peer|"
                r"network|pass\w*|token|key)\w*)\s*[=:].*$"), r"\1=<text>"),
    (re.compile(r"(?i)\b(from|named|called|channel|chan|via|ssid|node|contact|peer|sender|user|network|name|msg|"
                r"message|text|joined|connect(?:ed|ing)? to)\s+(?![<%s])(?:\S+:(?:\s.*)?$|\S+)" % M), r"\1 <word>"),
    (re.compile(r"(?<![\w:])[%s]{2}(?:[:-][%s]{2}){5}(?![\w:])" % (HEXSET, HEXSET)), "<addr>"),
    (re.compile(r"(?<![\w.])[%s]+(?:\.[%s]+){3}(?![\w.])" % (M, M)), "<addr>"),
    (re.compile(r"(?<![\w:])(?:[%s]{0,4}:){2,7}[%s]{0,4}(?![\w:])" % (HEXSET, HEXSET)), "<addr>"),
    (re.compile(r"[%s][xX][%s]+" % (M, HEXSET)), "<hex>"),
]
HEXRUN = re.compile(r"(?<![\w])[%s]{6,}(?![\w])" % HEXSET)
LONGTOK = re.compile(r"[A-Za-z%s+/=_-]{24,}" % M)
SLASHTOK = re.compile(r"\S*/\S*")
FINAL = re.compile(r"(?:(?<![\w%s.])-)?([%s])%s*(?:\.([%s])%s*)?" % (M, MARK, FILL, MARK, FILL))
UNSAFE = re.compile(r"""\d|['"@]|0x""")
_unmark = NUMBERED.sub


def _hexrun(m) -> str:
    """A token of 6+ hex characters (a number's fillers keep its length) is an identifier when it holds a hex letter
    and a number, or is 8+ letters."""
    s = m.group(0)
    letters = re.search("[a-fA-F]", s) is not None
    return "<hex>" if letters and (NUMBERED.search(s) or len(s) >= 8) else s


def _slash(m) -> str:
    return "<path>" if re.search("[A-Za-z]", m.group(0)) else m.group(0)


def scrub_marked(text: str) -> str:
    """Text whose numbers are marker chars -> the text with free text, addresses and identifiers replaced."""
    for rx, rep in _STEPS:
        text = rx.sub(rep, text)
    text = HEXRUN.sub(_hexrun, text)
    text = LONGTOK.sub("<tok>", text)
    return SLASHTOK.sub(_slash, text)


def numbered(text: str):
    """-> (marked text, digit strings): every run of digits replaced by its own private-use char."""
    raw = DIGITS.findall(text)
    it = iter(range(len(raw)))
    return DIGITS.sub(lambda m: chr(0xE000 + next(it)) + FILL * (len(m.group()) - 1), text), raw


def tokenise(marked: str):
    """-> (kind text with '#', [(negative, a, b)]) for the surviving numbers (b is the decimal part's marker)."""
    found = []

    def rep(m) -> str:
        a, b = ord(m.group(1)) - 0xE000, m.group(2)
        found.append((m.group(0).startswith("-"), a, None if b is None else ord(b) - 0xE000))
        return "#"
    return FINAL.sub(rep, marked), found


class Scrubber:
    """Text -> (kind text, numbers). One scrub per distinct digit-masked text; the cache maps that shape to the
    kind and to which of the line's digit runs survive as numbers, so a log of repeating lines costs few scrubs."""

    def __init__(self) -> None:
        self.cache: Dict[str, Tuple[str, list]] = {}
        self.parts: Dict[Tuple[str, str], str] = {}

    def text(self, text: str) -> Tuple[str, List[float]]:
        shape = DIGIT.sub("\uE000", text)   # one char per digit: a hex id's length decides how it is scrubbed
        hit = self.cache.get(shape)
        raw = DIGITS.findall(text)
        if hit is None:
            hit = self.fresh(text, len(raw))
            if len(self.cache) >= CACHE_MAX:
                self.cache.clear()
            self.cache[shape] = hit
        kind, idx = hit
        return kind, [float(("-" if n else "") + raw[a] + ("" if b is None else "." + raw[b])) for n, a, b in idx]

    def fresh(self, text: str, count: int) -> Tuple[str, list]:
        if count > MAX_NUMBERS:
            return "<oversized line>", []
        marked, _ = numbered(NUMBERED.sub("", text))
        kind, idx = tokenise(scrub_marked(marked))
        return re.sub(r"\s+", " ", kind).strip()[:160], idx

    def head(self, level: str, tag: str) -> str:
        key = (level, tag)
        if key not in self.parts:
            self.parts[key] = "%s %s" % (level, self.text(tag)[0]) if tag else level
        return self.parts[key]


def console_word(body: str) -> str:
    """Console echo, answers and ROM text are unstructured and may hold message text: only a first word shaped like
    a command or key (`soak1`, `>>`, `ESP-ROM:`) is kept."""
    word = (body.split(" ", 1) + [""])[0]
    return word if re.fullmatch(r"(?:>>|[a-z][a-z#_:-]{0,15})", word) else "<text>"


def split_line(line: str):
    """-> (wall seconds or None, level, ms since boot or None, tag, text). A line without the ESP-IDF form has level
    '-' (console echo, an answer, ROM text)."""
    if "\x1b" in line:
        line = ANSI.sub("", line)
    m = LINE.match(line.rstrip("\r\n"))
    wall = None if m.group(1) is None else int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
    if m.group(4) is None:
        return wall, "-", None, "", m.group(7)
    return wall, m.group(4), int(m.group(5)), m.group(6), m.group(7)


class Clock:
    """Run time in seconds from the first stamp, and the boot segments (a boot-relative stamp that drops)."""

    def __init__(self) -> None:
        self.t = 0.0
        self.t0: Optional[float] = None
        self.day = 0
        self.last_wall: Optional[int] = None
        self.anchor: Optional[Tuple[float, int]] = None   # (run time, ms) of the last line that had both
        self.prev_ms: Optional[int] = None
        self.base = 0.0
        self.segments: List[Tuple[int, float]] = []         # (line number, run time) where a new boot began

    def feed(self, no: int, wall: Optional[int], ms: Optional[int]) -> float:
        if ms is not None and self.prev_ms is not None and ms < self.prev_ms - 1000:
            self.segments.append((no, self.t))
            self.anchor, self.base = None, self.t
        if ms is not None:
            self.prev_ms = ms
        if wall is not None:
            t = self.from_wall(wall)
            if ms is not None:
                self.anchor = (t, ms)
        elif ms is not None:
            t = (self.anchor[0] + (ms - self.anchor[1]) / 1000.0) if self.anchor else self.base + ms / 1000.0
        else:
            t = self.t
        self.t = max(self.t, t)
        return self.t

    def from_wall(self, wall: int) -> float:
        if self.last_wall is not None and wall < self.last_wall - 43200:
            self.day += 1                     # past midnight; a smaller step back is a clock step and is held
        self.last_wall = wall
        t = wall + self.day * 86400.0
        if self.t0 is None:
            self.t0 = t - self.t
        return t - self.t0


class Acc:
    """Count, maximum and a bounded sample (for the median) of one number in one window."""
    __slots__ = ("n", "mx", "vals")

    def __init__(self) -> None:
        self.n, self.mx, self.vals = 0, None, []

    def add(self, v: float, rng: random.Random) -> None:
        self.n += 1
        self.mx = v if self.mx is None or v > self.mx else self.mx
        if len(self.vals) < SAMPLE_CAP:
            self.vals.append(v)
        else:
            j = rng.randrange(self.n)
            if j < SAMPLE_CAP:
                self.vals[j] = v


class Kind:
    __slots__ = ("first", "count", "wins", "nums")

    def __init__(self, first: float) -> None:
        self.first, self.count = first, 0
        self.wins: Dict[int, int] = {}
        self.nums: Dict[Tuple[int, int], Acc] = {}


class Census:
    def __init__(self, warmup_s: float, window_s: float, gap_s: float) -> None:
        self.warmup, self.window, self.gap = warmup_s, window_s, gap_s
        self.scrub, self.clock = Scrubber(), Clock()
        self.kinds: Dict[str, Kind] = {}
        self.lines, self.last_t, self.gaps = 0, 0.0, []
        self.rng = random.Random(430)

    def window_of(self, t: float) -> int:
        return -1 if t < self.warmup else int((t - self.warmup) // self.window)

    def add(self, line: str) -> None:
        self.lines += 1
        wall, level, ms, tag, text = split_line(line)
        t = self.clock.feed(self.lines, wall, ms)
        if t - self.last_t > self.gap:
            self.gaps.append((self.lines, self.last_t, t - self.last_t))
        self.last_t = t
        body, nums = self.scrub.text(text)
        if level == "-":
            body = console_word(body)
        name = "%s: %s" % (self.scrub.head(level, tag), body)
        k = self.kinds.get(name)
        if k is None:
            k = self.kinds.get("<other kinds>") if len(self.kinds) >= MAX_KINDS else None
            if k is None:
                k = self.kinds[name if len(self.kinds) < MAX_KINDS else "<other kinds>"] = Kind(t)
        w = self.window_of(t)
        k.count += 1
        k.wins[w] = k.wins.get(w, 0) + 1
        for i, v in enumerate(nums):
            k.nums.setdefault((w, i), Acc()).add(v, self.rng)


def read_census(lines: Iterable[str], warmup_s: float, window_s: float, gap_s: float) -> Census:
    c = Census(warmup_s, window_s, gap_s)
    for line in lines:
        c.add(line)
    return c


def hours(t: float) -> str:
    return "%.1f h" % (t / 3600.0)


def usable_windows(c: Census) -> Tuple[int, Dict[int, float]]:
    """-> (last window index, {window: seconds covered}) of the windows after the warm-up. A last window covering less
    than half a window is dropped (its rate would be noise); one more than half is kept and its rate scaled."""
    last = c.window_of(c.last_t)
    if last < 0:
        return -1, {}
    span = c.last_t - (c.warmup + last * c.window)
    if span < c.window / 2 and last > 0:
        last -= 1
        span = c.window
    return last, {w: (min(span, c.window) if w == last else c.window) for w in range(last + 1)}


def late_kinds(c: Census) -> List[Tuple[str, Kind]]:
    found = [(n, k) for n, k in c.kinds.items() if k.first >= c.warmup and n != "<other kinds>"]
    return sorted(found, key=lambda nk: nk[1].first)


def rising_rate(k: Kind, last: int, dur: Dict[int, float]) -> Optional[Tuple[float, float]]:
    """(first, last window rate per hour) when the kind's rate rises over the windows after the warm-up."""
    if last < 1 or k.wins.get(-1, 0) == 0:   # no warm-up line: a late kind, listed on its own
        return None
    r0, rl = k.wins.get(0, 0) / dur[0] * 3600, k.wins.get(last, 0) / dur[last] * 3600
    extra = k.wins.get(last, 0) - k.wins.get(0, 0) * dur[last] / dur[0]
    if rl < 2 * r0 or extra < 3:
        return None
    if last >= 2:
        xs = list(range(last + 1))
        if soak_trend.theil_sen(xs, [k.wins.get(w, 0) / dur[w] for w in xs])[0] <= 0:
            return None
    return r0, rl


def rising_numbers(k: Kind, last: int) -> List[Tuple[int, str, float, float]]:
    """[(position, 'median'|'max', first, last)] for the numbers of a kind that rise between the first and the last
    window after the warm-up that hold enough values."""
    out = []
    for i in sorted({i for (_, i) in k.nums}):
        full = [w for w in range(last + 1) if k.nums.get((w, i)) and k.nums[(w, i)].n >= MIN_VALUES]
        if len(full) < 2:
            continue
        a, b = k.nums[(full[0], i)], k.nums[(full[-1], i)]
        for what, x, y in (("median", statistics.median(a.vals), statistics.median(b.vals)), ("max", a.mx, b.mx)):
            if y - x >= max(1.0, (RISE - 1) * abs(x)):   # abs: a negative figure (dBm) rising from -80 to -78 is not 25%
                out.append((i, what, x, y))
    return out


def safe_kind(name: str) -> str:
    """Fail closed: a kind that still holds a digit, quote or `@` after scrubbing is not printed."""
    return "<withheld kind>" if UNSAFE.search(name) else name


def limited(title: str, rows: List[str], top: int) -> List[str]:
    return ["%s: %d" % (title, len(rows))] + ["  " + r for r in rows[:top]] + (
        ["  ... and %d more" % (len(rows) - top)] if len(rows) > top else [])


def census_report(c: Census, top: int = 40) -> List[str]:
    last, dur = usable_windows(c)
    out = ["log census: %d lines, %d kinds, %s of log, warm-up %s, window %s" % (
        c.lines, len(c.kinds), hours(c.last_t), hours(c.warmup), hours(c.window))]
    out += ["  boot restarts (the boot-relative stamp dropped): %d%s" % (
        len(c.clock.segments), "".join("; line %d at %s" % (n, hours(t)) for n, t in c.clock.segments[:10]))]
    out += ["  log gaps over %.0f min: %d%s" % (c.gap / 60.0, len(c.gaps), "".join(
        "; %s at line %d" % (hours(g), n) for n, _, g in c.gaps[:10]))]
    if last < 1:
        return out + ["  the log is shorter than the warm-up plus two windows: no rate or number trend checks"]
    out += limited("kinds first seen after the warm-up", [
        "%s  (first at %s, %d lines)" % (safe_kind(n), hours(k.first), k.count) for n, k in late_kinds(c)], top)
    rates = [(n, k, r) for n, k in c.kinds.items() for r in [rising_rate(k, last, dur)] if r and n != "<other kinds>"]
    out += limited("kinds whose rate rises", [
        "%s  (%.1f -> %.1f per hour)" % (safe_kind(n), r[0], r[1]) for n, k, r in sorted(rates, key=lambda x: -x[2][1])], top)
    nums = [(n, i, w, x, y) for n, k in c.kinds.items() for i, w, x, y in rising_numbers(k, last)]
    out += limited("numbers that rise (median or max, first window after warm-up -> last)", [
        "%s  number %d %s %s -> %s" % (safe_kind(n), i + 1, w, soak_trend.num(x), soak_trend.num(y))
        for n, i, w, x, y in sorted(nums, key=lambda r: -(r[4] - r[3]))], top)
    return out
