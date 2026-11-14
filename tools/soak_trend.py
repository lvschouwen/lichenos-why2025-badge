"""Trend test over the soak sampler's CSV (#430, #407 check B), used by `soak_report.py trend <samples.csv>`.
For every numeric column: the Theil-Sen slope over the samples after the warm-up and the median of the last quarter
against the median of the first quarter, judged against the sample noise (1.4826 x MAD of the residuals around the
fitted line). A column that moves is reported with both medians unless the allow file (tools/soak_rising.txt) lists it as
meant to rise. Report only: nothing here fails a soak. A gap in the samples and a reboot are reported; the fit uses
the longest boot segment only, so a counter that restarted is never fitted through. Standard library only."""
import math
import os
import re
import statistics
import sys
from typing import Dict, List, Optional, Sequence, Set, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_parse  # noqa: E402

DEFAULT_ALLOW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "soak_rising.txt")
COMMENT = re.compile(r"(?:^|\s)#(?:\s|$)")
TIME_KEYS = ("t", "ts", "time", "epoch", "unix", "timestamp")  # a numeric clock column, seconds
MIN_POINTS = 8       # fewer values after the warm-up and no trend is claimed
SIGMAS = 3.0         # a quarter-to-quarter move must exceed this many standard errors of the noise
GAP_FACTOR = 2.0     # a step longer than this many median steps is a gap
PAIR_CAP = 400       # Theil-Sen pairs grow with n squared: beyond this many points the series is thinned


def load_allow(path: str) -> Dict[str, str]:
    """Allow file: one key per line, then an optional reason; `# ` starts a comment. -> {key: reason}."""
    out: Dict[str, str] = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            cols = COMMENT.split(raw, 1)[0].split(None, 1)
            if cols:
                out[cols[0]] = cols[1].strip() if len(cols) > 1 else ""
    return out


def theil_sen(xs: Sequence[float], ys: Sequence[float]) -> Tuple[float, float]:
    """-> (slope, intercept): the median of all pairwise slopes and the median offset. Robust to a few spikes."""
    step = max(1, len(xs) // PAIR_CAP)
    px, py = list(xs[::step]), list(ys[::step])
    slopes = [(py[j] - py[i]) / (px[j] - px[i]) for i in range(len(px)) for j in range(i + 1, len(px))
              if px[j] != px[i]]
    slope = statistics.median(slopes) if slopes else 0.0
    return slope, statistics.median(y - slope * x for x, y in zip(xs, ys))


def noise(xs: Sequence[float], ys: Sequence[float], slope: float, icpt: float) -> float:
    res = [y - slope * x - icpt for x, y in zip(xs, ys)]
    med = statistics.median(res)
    return 1.4826 * statistics.median(abs(r - med) for r in res)


def derive_times(rows: List[soak_parse.Sample], interval: float) -> Tuple[List[float], str]:
    """Seconds of every sample and where they came from: a clock column in every row, else the sampler's `up` (a
    reboot counts one nominal step), else the index times the nominal interval."""
    for key in TIME_KEYS:
        vals = [soak_parse.value(s, key) for s in rows]
        if rows and all(v is not None for v in vals):
            return [float(v) for v in vals], key
    ups = [soak_parse.value(s, "up") for s in rows]
    if not rows or any(u is None for u in ups):
        return [i * interval for i in range(len(rows))], "index"
    out = [0.0]
    for a, b in zip(ups, ups[1:]):
        out.append(out[-1] + ((b - a) if b > a else interval))
    return out, "up"


def find_gaps(times: List[float], reboot_at: Set[int]) -> List[Tuple[int, float]]:
    """-> [(sample position, seconds)] where a step is much longer than the median step (a reboot step is not a gap)."""
    steps = [b - a for a, b in zip(times, times[1:])]
    pos = [s for i, s in enumerate(steps) if (i + 1) not in reboot_at and s > 0]
    if not pos:
        return []
    med = statistics.median(pos)
    return [(i + 1, s) for i, s in enumerate(steps) if (i + 1) not in reboot_at and s > GAP_FACTOR * med]


def split_boots(count: int, reboot_at: Set[int]) -> List[range]:
    cuts = [0] + sorted(reboot_at) + [count]
    return [range(a, b) for a, b in zip(cuts, cuts[1:]) if b > a]


def series(rows: List[soak_parse.Sample], times: List[float], keep: Sequence[int], skip: Set[str]):
    """{column: ([hours], [values])} over the kept sample positions; unknown (-1) and absent cells are left out."""
    cols: Dict[str, Tuple[List[float], List[float]]] = {}
    for k in keep:
        for key in rows[k]:
            if key in skip:
                continue
            v = soak_parse.value(rows[k], key)
            if v is not None:
                xs, ys = cols.setdefault(key, ([], []))
                xs.append(times[k] / 3600.0)
                ys.append(float(v))
    return cols


def judge(xs: List[float], ys: List[float]) -> Optional[dict]:
    """The quarter and slope tests of one column, or None when it does not move (or has too few values)."""
    if len(ys) < MIN_POINTS:
        return None
    slope, icpt = theil_sen(xs, ys)
    sigma = noise(xs, ys, slope, icpt)
    q = len(ys) // 4
    first, last = statistics.median(ys[:q]), statistics.median(ys[-q:])
    delta = last - first
    span = xs[-1] - xs[0]
    # Either test fires: the quarters (a step that starts late leaves the slope at 0) or the fitted drift over the run
    # (a ramp whose quarters differ little). With no noise at all (sigma 0) any change counts.
    if abs(delta) <= SIGMAS * sigma * math.sqrt(2.0 / q) and abs(slope * span) <= SIGMAS * sigma:
        return None
    return {"first": first, "last": last, "slope": slope, "noise": sigma, "up": (delta if delta else slope) > 0}


def num(v: float) -> str:
    return "%d" % v if float(v).is_integer() else "%.3g" % v


def move_line(key: str, m: dict) -> str:
    return "  %-14s %s  first quarter median %s -> last %s  (slope %s/h, noise %s)" % (
        key, "rises" if m["up"] else "falls", num(m["first"]), num(m["last"]), num(m["slope"]),
        num(m["noise"]))


def trend_report(samples: List[soak_parse.Sample], allow: Dict[str, str], warmup_h: float = 2.0,
                 interval_s: float = 600.0) -> List[str]:
    """The report lines of the trend test over a sampler CSV (see the module doc)."""
    rows = soak_parse.usable(samples)
    out = ["trend test: %d samples, %d incomplete (left out)" % (len(samples), len(samples) - len(rows))]
    if not rows:
        return out + ["  no usable samples"]
    times, source = derive_times(rows, interval_s)
    reboot_at = set(soak_parse.reboots(rows))
    out.append("  time from: %s; warm-up %g h" % (source, warmup_h))
    for k in sorted(reboot_at):
        out.append("  reboot before sample %d (about %.1f h into the run)" % (k, times[k] / 3600.0))
    for k, s in find_gaps(times, reboot_at):
        out.append("  gap of %.1f min before sample %d (typical step %.1f min)" % (
            s / 60.0, k, statistics.median(b - a for a, b in zip(times, times[1:])) / 60.0))
    return out + fit_longest(rows, times, reboot_at, allow, warmup_h)


def fit_longest(rows, times, reboot_at, allow, warmup_h) -> List[str]:
    """The fit over the longest boot segment after its warm-up; the other segments are named, not fitted."""
    segs = split_boots(len(rows), reboot_at)
    seg = max(segs, key=len)
    if len(segs) > 1:
        out = ["  boot segments: %d; fitted: samples %d-%d (the longest), the others are not fitted through" % (
            len(segs), seg.start, seg.stop - 1)]
    else:
        out = []
    keep = [k for k in seg if times[k] - times[seg.start] >= warmup_h * 3600.0]
    skip = set(TIME_KEYS) | {"incomplete", "v"}
    cols = series(rows, times, keep, skip)
    out.append("  after warm-up: %d samples in the fitted segment" % len(keep))
    moving, allowed = [], []
    for key in sorted(cols):
        m = judge(*cols[key])
        if m is None:
            continue
        (allowed if key in allow and m["up"] else moving).append((key, m))
    out.append("  moving (not on the allow list): %d" % len(moving))
    out += [move_line(k, m) for k, m in moving]
    out.append("  rising as allowed: %d%s" % (len(allowed), " (%s)" % ", ".join(k for k, _ in allowed) if allowed else ""))
    short = sorted(k for k, (xs, _) in cols.items() if len(xs) < MIN_POINTS)
    if short:
        out.append("  too few values to judge: %s" % ", ".join(short))
    if len(keep) < MIN_POINTS:
        out.append("  fewer than %d samples after the warm-up: no trend claimed" % MIN_POINTS)
    return out
