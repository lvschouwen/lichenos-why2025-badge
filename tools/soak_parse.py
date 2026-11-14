"""Parser for `fw soak` replies (#424, #407), shared by the soak tools. The contract is firmware/main/soak_line.h:
four lines soak1..soak4 of key=value words, -1 = unknown, a lower `boot` or `up` than the last sample = a reboot.
Standard library only; no badge access."""
import csv

NA = -1
LINES = 4


def parse_line(line):
    """One `soakN v=1 key=value ...` line -> (N, {key: int or str}); None for any other line.
    A number stays an int (-1 stays -1: use value() to turn it into None), an enum word stays a str."""
    words = line.split()
    if not words or not words[0].startswith("soak") or not words[0][4:].isdigit():
        return None
    out = {}
    for w in words[1:]:
        key, eq, val = w.partition("=")
        if not eq or not key:
            continue
        try:
            out[key] = int(val)
        except ValueError:
            out[key] = val
    return int(words[0][4:]), out


def parse_reply(text):
    """All soak lines of a console transcript -> a list of samples, one dict per `fw soak` (soak1 starts a sample).
    The key sets of the four lines are merged; boot and up repeat on every line and the first line's win. A sample
    missing a line is kept (its keys are simply absent: a missing key is not a zero)."""
    samples = []
    for line in text.splitlines():
        parsed = parse_line(line)
        if parsed is None:
            continue
        n, kv = parsed
        if n == 1 or not samples:
            samples.append({})
        for k, v in kv.items():
            samples[-1].setdefault(k, v)
    return samples


def value(sample, key):
    """The key's number, or None when it is absent, -1 (unknown) or not a number (an enum word, an empty CSV cell)."""
    v = sample.get(key)
    if isinstance(v, str):
        try:
            v = int(v)
        except ValueError:
            try:
                v = float(v)
            except ValueError:
                return None
    if v is None or v == NA:
        return None
    return v


def reboots(samples):
    """Indexes i where sample i follows a reboot of sample i-1: boot higher, or boot or up LOWER (the counter's storage
    was restarted, which counts as a reboot too). Unknown boot or up (-1) is no change."""
    found = []
    for i in range(1, len(samples)):
        a, b = samples[i - 1], samples[i]
        for key in ("boot", "up"):
            x, y = value(a, key), value(b, key)
            if x is not None and y is not None and ((key == "boot" and y > x) or y < x):
                found.append(i)
                break
    return found


def _number(text):
    for conv in (int, float):
        try:
            return conv(text)
        except ValueError:
            pass
    return text


def read_csv(path):
    """A CSV of samples (a header of keys, one row per sample) -> a list of dicts; numbers become ints or floats, an
    empty cell is left out, anything else stays a string."""
    rows = []
    with open(path, newline="") as f:
        for raw in csv.DictReader(f):
            row = {}
            for k, v in raw.items():
                if k is None or v is None or v == "":
                    continue
                row[k] = _number(v)
            rows.append(row)
    return rows


def write_csv(path, samples):
    keys = []
    for s in samples:
        keys += [k for k in s if k not in keys]
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(samples)
