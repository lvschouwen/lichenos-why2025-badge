"""Parser for `fw soak` replies (#424, #407), shared by the soak tools. The contract is firmware/main/soak_line.h:
four lines soak1..soak4 of key=value words, -1 = unknown, a lower `boot` or `up` than the last sample = a reboot.
Standard library only; no badge access."""
import csv
import math
import re
from typing import Dict, List, Optional, Union

NA = -1
LINES = 4
Value = Union[int, float, str]
Sample = Dict[str, Value]

# A console transcript prefixes lines ("fw: soak1 ...", "12:00:01 soak1 ...", ">> soak1 ..."): find the line by its
# name and its first key (v=), and parse from there.
_START = re.compile(r"\bsoak(\d)\s+v=")
_COMMON = ("v", "boot", "up")  # on every line: they say which boot and moment the line belongs to


def parse_line(line: str):
    """One `soakN v=1 key=value ...` line (any prefix before it is ignored) -> (N, {key: int or str}); None for any
    other line. A number stays an int (-1 stays -1: use value() to turn it into None), an enum word stays a str. A key
    repeated within the line keeps its first value."""
    m = _START.search(line)
    if m is None:
        return None
    out: Dict[str, Value] = {}
    for w in line[m.start():].split()[1:]:
        key, eq, val = w.partition("=")
        if not eq or not key or key in out:
            continue
        try:
            out[key] = int(val)
        except ValueError:
            out[key] = val
    return int(m.group(1)), out


def _consistent(first: Dict[str, Value], other: Dict[str, Value]) -> bool:
    return all(first.get(k) == other.get(k) for k in _COMMON)


def parse_reply(text: str) -> List[Sample]:
    """All soak lines of a console transcript -> a list of samples, one dict per `fw soak`: the key sets of the four
    lines merged. A new sample starts when a line number is not higher than the previous line's (so a lost soak1
    never merges its soak2 into the sample before). A sample that lacks a line, or whose lines disagree on v, boot or
    up (two replies interleaved), carries `incomplete=1` and must not be used for figures (see usable())."""
    samples: List[Sample] = []
    prev = 0
    seen: List[set] = []
    firsts: List[Dict[str, Value]] = []
    bad: List[bool] = []
    for line in text.splitlines():
        parsed = parse_line(line)
        if parsed is None:
            continue
        n, kv = parsed
        if n <= prev or not samples:
            samples.append({})
            seen.append(set())
            firsts.append(kv)
            bad.append(False)
        elif not _consistent(firsts[-1], kv):
            bad[-1] = True
        prev = n
        seen[-1].add(n)
        for k, v in kv.items():
            samples[-1].setdefault(k, v)
    for s, got, flag in zip(samples, seen, bad):
        if flag or got != set(range(1, LINES + 1)):
            s["incomplete"] = 1
    return samples


def usable(samples: List[Sample]) -> List[Sample]:
    """The samples whose four lines arrived together: the only ones figures are taken from."""
    return [s for s in samples if s.get("incomplete") != 1]


def value(sample: Sample, key: str) -> Optional[Union[int, float]]:
    """The key's number, or None when it is absent, -1 (unknown), nan/inf or not a number (an enum word)."""
    v = sample.get(key)
    if isinstance(v, str):
        v = _number(v)
    if not isinstance(v, (int, float)) or v == NA:
        return None
    if isinstance(v, float) and not math.isfinite(v):
        return None
    return v


def reboots(samples: List[Sample]) -> List[int]:
    """Indexes i where sample i follows a reboot: boot higher, or boot or up LOWER than the last KNOWN value (the
    counter's storage was restarted, which counts as a reboot too). Unknown (-1) values and incomplete samples are
    skipped, never reset what was last known."""
    found = []
    last = {"boot": None, "up": None}
    for i, s in enumerate(samples):
        if s.get("incomplete") == 1:
            continue
        hit = False
        for key in ("boot", "up"):
            v = value(s, key)
            if v is None:
                continue
            prev = last[key]
            if prev is not None and ((key == "boot" and v > prev) or v < prev):
                hit = True
            last[key] = v
        if hit:
            found.append(i)
    return found


def _number(text: str) -> Value:
    for conv in (int, float):
        try:
            return conv(text)
        except ValueError:
            pass
    return text


def read_csv(path: str) -> List[Sample]:
    """A CSV of samples (a header of keys, one row per sample) -> a list of dicts; numbers become ints or floats, an
    empty cell or a nan/inf is left out (unknown), anything else stays a string."""
    rows = []
    with open(path, newline="", encoding="utf-8-sig", errors="replace") as f:
        for raw in csv.DictReader(f):
            row: Sample = {}
            for k, v in raw.items():
                if k is None or v is None or v == "":
                    continue
                n = _number(v)
                if isinstance(n, float) and not math.isfinite(n):
                    continue
                row[k] = n
            rows.append(row)
    return rows


def write_csv(path: str, samples: List[Sample]) -> None:
    """Write samples as a CSV that read_csv reads back: the union of all keys in first-seen order as the header, an
    empty cell where a sample lacks a key."""
    keys: List[str] = []
    for s in samples:
        keys += [k for k in s if k not in keys]
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(samples)
