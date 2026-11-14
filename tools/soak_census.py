"""Log census of a serial log (#430, #407 check A), used by `soak_report.py census <serial-log>`.
Every line becomes a KIND: level, tag and the text with its numbers taken out. Kinds are counted per window (default
2 h) after a warm-up (default 2 h). Flagged, report only (no pass limit): a kind that first appears after the warm-up;
a kind whose rate rises (last window against the first after the warm-up); and, for the numbers of a kind that carry
a unit or a safe `key=`, a median or maximum that rises between the first window after the warm-up and the last.
Timestamps: the flash host's logger writes `HH:MM:SS ` (tools/serial_log.py, local clock, midnight wraps, a small
step back is held); a line's ESP-IDF `I (12345) tag:` stamp is milliseconds since boot. A new boot segment (reported)
starts when that stamp drops, and at a ROM boot banner (`ESP-ROM:`, `rst:0x..`, `boot:0x..`). Without wall stamps
the boot-relative stamps are chained across segments.
The log is read as a stream; per kind and window only a bounded sample of numbers is kept.
SCRUBBING IS AN ALLOW-LIST: a kind is printed only from the vocabulary of the firmware's own log words
(built from the source tree at run time by tools/soak_vocab.py, or read from `--vocab FILE`); every other token, any token with a capital past its first
letter, a digit or a non-ASCII character, quoted text and what follows `<unknown>:` is printed as `<w>`. Numbers are
printed as `#`; their medians and maxima are shown only for a number followed by a unit (ms us s B KB MB % dBm mV mA C)
or the value of a vocabulary `key=` outside a small deny list (lat, lon, pos, key, id, ip ...). Residual gap: a name
that is also a firmware word is printed as that word. Standard library only."""
import os
import random
import re
import statistics
import sys
from typing import Dict, Iterable, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_trend  # noqa: E402
import soak_vocab  # noqa: E402

NUMCH = "-"        # a number is one private-use char plus fillers (one per further digit) in a line
UNITCH = "-"       # a unit that directly follows a number
FILL = ""
LETTER = "A-Za-z\u0080-\uDFFF豈-\U0010FFFF"
DIGITS = re.compile(r"\d+")
DIGIT = re.compile(r"\d")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
LINE = re.compile(r"^(?:(\d\d):(\d\d):(\d\d) )?(?:([EWIDV]) \((\d+)\) ([^\s:]+): ?)?(.*)$")
BANNER = re.compile(r"^ESP-ROM:|\brst:0x[0-9a-fA-F]+|\bboot:0x[0-9a-fA-F]+")
MAX_NUMBERS = 6000
MAX_KINDS = 20000
MAX_POSITIONS = 8
CACHE_MAX = 200000
SAMPLE_CAP = 512
MIN_VALUES = 3
RISE = 1.25
UNITS = ("ms", "us", "s", "B", "KB", "MB", "%", "dBm", "mV", "mA", "C")
DENY_KEYS = frozenset(("lat", "lon", "pos", "key", "id", "ip", "mac", "addr", "port", "chan", "freq"))
QUOTED = re.compile(r'"(?:[^"\\]|\\.)*(?:"|$)')
FREE_KV = re.compile(r"(?i)\b((?:name|ssid|node|contact|nick|sender|user|msg|message|text|payload|body|title|peer|"
                     r"network|pass\w*|token|key)\w*)\s*[=:].*$")
FROM_WORD = re.compile(r"(?i)\b(from|named|called|channel|chan|via|joined|connect(?:ed|ing)? to)\s+"
                       r"(?=[A-Za-z\u0080-\U0010FFFF])\S+(?:\s+(?=[A-Z\u0080-\U0010FFFF])\S+)*")
NUMUNIT = re.compile("([%s])%s*( ?)(%s)(?![%s%s%s])" % (
    NUMCH, FILL, "|".join(re.escape(u) for u in UNITS), LETTER, NUMCH, FILL))
TOKEN = re.compile("[%s%s%s_]+" % (LETTER, NUMCH, FILL))
FINAL = re.compile(r"(?:(?<![\w%s.])-)?([%s])%s*(?:\.([%s])%s*)?(?:( ?)([%s]))?" % (
    NUMCH, NUMCH, FILL, NUMCH, FILL, UNITCH))
KEY_BEFORE = re.compile(r"([a-z]+)=$")
UNK = "\x01"                    # an unknown token while a line is scrubbed; printed as <w>
AFTER_UNKNOWN = re.compile(UNK + r"[:\]].*$")
W_RUN = re.compile(UNK + r"(?:\s+" + UNK + ")+")
UNSAFE = re.compile(r"""[^\x20-\x7e]|\d|['"@]|0x""")


def word_ok(tok: str, vocab) -> bool:
    """A token is printable when it is lowercase ASCII (a capital only first) and every `_` part is a vocabulary word."""
    if not tok.isascii() or not tok.replace("_", "a").isalpha() or tok[1:] != tok[1:].lower():
        return False
    return all(p.lower() in vocab for p in tok.split("_"))


def classify(vocab):
    def rep(m) -> str:
        tok = m.group(0)
        if re.fullmatch("[%s]%s*" % (NUMCH, FILL), tok):
            return tok                                  # a number: FINAL takes it out later
        if re.search("[%s]" % NUMCH, tok) or not word_ok(tok, vocab):
            return UNK                                  # a digit anywhere, or not a vocabulary word
        return tok
    return rep


def numbered(text: str):
    """-> (marked text, digit strings): every run of digits replaced by its own private-use char plus fillers."""
    raw = DIGITS.findall(text)
    it = iter(range(len(raw)))
    return DIGITS.sub(lambda m: chr(0xE000 + next(it)) + FILL * (len(m.group()) - 1), text), raw


def mark_units(marked: str) -> str:
    return NUMUNIT.sub(lambda m: m.group(1) + m.group(2) + chr(0xF8F0 + UNITS.index(m.group(3))), marked)


def tokenise(text: str, vocab):
    """-> (kind text with '#', [(negative, a, b, show)]) for the surviving numbers. `show`: the number may have its
    statistics printed (a unit follows, or it is the value of a vocabulary key= outside DENY_KEYS)."""
    found = []

    def rep(m) -> str:
        a, b = ord(m.group(1)) - 0xE000, m.group(2)
        key = KEY_BEFORE.search(text[:m.start()])
        show = m.group(4) is not None or (key is not None and key.group(1) in vocab and key.group(1) not in DENY_KEYS)
        found.append((m.group(0).startswith("-"), a, None if b is None else ord(b) - 0xE000, show))
        unit = "" if m.group(4) is None else m.group(3) + UNITS[ord(m.group(4)) - 0xF8F0]
        return "#" + unit
    return FINAL.sub(rep, text).replace(UNK, "<w>"), found


def scrub_marked(marked: str, vocab) -> str:
    """Marked text (numbers and units as private-use chars) -> text of vocabulary words, `#` slots and `<w>`."""
    text = QUOTED.sub(UNK, marked)
    text = FREE_KV.sub("\\1=" + UNK, text)
    text = FROM_WORD.sub("\\1 " + UNK, text)
    text = TOKEN.sub(classify(vocab), mark_units(text))
    return W_RUN.sub(UNK, AFTER_UNKNOWN.sub(lambda m: m.group(0)[:2], text))


class Scrubber:
    """Text -> (kind text, [(value, show)]). One scrub per distinct digit-masked text; the cache maps that shape to
    the kind and to which of the line's digit runs survive as numbers, so a log of repeating lines costs few
    scrubs."""

    def __init__(self, vocab=None) -> None:
        self.vocab = soak_vocab.build() if vocab is None else frozenset(vocab)
        self.cache: Dict[str, Tuple[str, list]] = {}
        self.parts: Dict[Tuple[str, str], str] = {}

    def text(self, text: str) -> Tuple[str, List[Tuple[Optional[float], bool]]]:
        shape = DIGIT.sub("", text)   # one char per digit: a number's length decides how a token is judged
        hit = self.cache.get(shape)
        raw = DIGITS.findall(text)
        if hit is None:
            hit = self.fresh(text, len(raw))
            if len(self.cache) >= CACHE_MAX:
                self.cache.clear()
            self.cache[shape] = hit
        kind, idx = hit
        return kind, [(float(("-" if n else "") + raw[a] + ("" if b is None else "." + raw[b])) if show else None, show)
                      for n, a, b, show in idx]

    def fresh(self, text: str, count: int) -> Tuple[str, list]:
        if count > MAX_NUMBERS:
            return "<w>", []
        marked, _ = numbered(re.sub("[-]", "", text))
        kind, idx = tokenise(scrub_marked(marked, self.vocab), self.vocab)
        return re.sub(r"\s+", " ", kind).strip()[:160], idx

    def head(self, level: str, tag: str) -> str:
        key = (level, tag)
        if key not in self.parts:
            self.parts[key] = "%s %s" % (level, self.text(tag)[0]) if tag else level
        return self.parts[key]

    def console(self, body: str) -> str:
        """Console echo, answers and ROM text: the same allow-list; an echoed command keeps only its first word."""
        if body.startswith(">>"):
            first = (body[2:].split(None, 1) + [""])[0]
            return ">> " + self.text(first)[0] + (" <w>" if body[2:].strip() != first else "")
        return self.text(body)[0]


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

    def restart(self, no: int) -> None:
        """A new boot began at this line (a ROM banner): its stamps start from zero again."""
        self.segments.append((no, self.t))
        self.anchor, self.base, self.prev_ms = None, self.t, None

    def feed(self, no: int, wall: Optional[int], ms: Optional[int]) -> float:
        if ms is not None and self.prev_ms is not None and ms < self.prev_ms - 1000:
            self.restart(no)
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
    def __init__(self, warmup_s: float, window_s: float, gap_s: float, vocab=None) -> None:
        self.warmup, self.window, self.gap = warmup_s, window_s, gap_s
        self.scrub, self.clock = Scrubber(vocab), Clock()
        self.in_banner = False
        self.kinds: Dict[str, Kind] = {}
        self.lines, self.last_t, self.gaps = 0, 0.0, []
        self.rng = random.Random(430)

    def window_of(self, t: float) -> int:
        return -1 if t < self.warmup else int((t - self.warmup) // self.window)

    def boot_banner(self, text: str, stamped_before: bool) -> None:
        """The ROM prints its banner on every reset, before any stamped line: one segment per banner block (not for
        the banner that opens the log)."""
        banner = BANNER.search(text) is not None
        if banner and not self.in_banner and stamped_before:
            self.clock.restart(self.lines)
        self.in_banner = banner

    def add(self, line: str) -> None:
        self.lines += 1
        wall, level, ms, tag, text = split_line(line)
        stamped_before = self.clock.t0 is not None or self.clock.prev_ms is not None
        t = self.clock.feed(self.lines, wall, ms)
        if t - self.last_t > self.gap:
            self.gaps.append((self.lines, self.last_t, t - self.last_t))
        self.last_t = t
        if level == "-":
            self.boot_banner(text, stamped_before)
            body, nums = self.scrub.console(text), []
        else:
            body, nums = self.scrub.text(text)
        name = "%s: %s" % (self.scrub.head(level, tag), body)
        k = self.kinds.get(name)
        if k is None:
            k = self.kinds.get("<other kinds>") if len(self.kinds) >= MAX_KINDS else None
            if k is None:
                k = self.kinds[name if len(self.kinds) < MAX_KINDS else "<other kinds>"] = Kind(t)
        w = self.window_of(t)
        k.count += 1
        k.wins[w] = k.wins.get(w, 0) + 1
        for i, (v, show) in enumerate(nums[:MAX_POSITIONS]):
            if show:
                k.nums.setdefault((w, i), Acc()).add(v, self.rng)


def read_census(lines: Iterable[str], warmup_s: float, window_s: float, gap_s: float, vocab=None) -> Census:
    c = Census(warmup_s, window_s, gap_s, vocab)
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
