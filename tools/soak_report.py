#!/usr/bin/env python3
"""Soak report (#424, #430, #407). Three parts:
`soak_report.py census <serial.log> [--warmup-h 2] [--window-h 2] [--gap-min 30] [--top 40]` counts every log line kind
per window and reports late kinds, rising rates and rising numbers (tools/soak_census.py);
`soak_report.py trend <samples.csv> [--warmup-h 2] [--interval-s 600] [--allow FILE]` tests every numeric column for a
drift, with the allow file tools/soak_rising.txt (tools/soak_trend.py). Both are report only: exit 0, or 2 when the
input cannot be read.

`soak_report.py expected <samples.csv> [--log <serial.log>] [--budgets FILE]`.
Prints the expected-against-measured table of tools/soak_budgets.txt (format documented there): figure, expected,
tolerance, measured median, measured worst, flag. Samples marked incomplete (a lost or mismatched soak line) are
counted and left out of the figures.
Flags: ok; over / under (a TWO-SIDED figure outside its tolerance, either direction: `under` can mean the feature is
not running); breach (a ONE-SIDED limit, <= >= < >, violated, in whichever direction); unplanned (measured only, no
plan); no data.
Exit status: 0 nothing flagged, 1 flagged (over, under, breach, or no data on a soak-key figure: a planned key the CSV
lacks is a broken sampler), 2 a tool error (unreadable file, bad budget file: the message names path:line).
`unplanned`, and `no data` on a probe or log figure (including every log figure when --log is not given), are shown
and counted in the summary but do not fail the run.
Worst = the value furthest past the expectation (min for >= and >, max for <= and <, farthest from expected for a
tolerance, max for unplanned). Standard library only."""
import os
import re
import statistics
import sys
from typing import Dict, List, Optional, Sequence, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_census  # noqa: E402
import soak_parse  # noqa: E402
import soak_trend  # noqa: E402

DEFAULT_BUDGETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "soak_budgets.txt")
# A comment is a "#" followed by a space or the line end: "#234" (an issue) is a source, not a comment.
COMMENT = re.compile(r"(?:^|\s)#(?:\s|$)")
KINDS = ("key", "log", "probe", "unplanned")
EXPECTED = re.compile(r"^(<=|>=|<|>)?(-?\d+(?:\.\d+)?)$")
TOLERANCE = re.compile(r"^(\d+(?:\.\d+)?)(%?)$")
FORMAT = "figure kind measure expected tolerance unit source [note]"


class BudgetError(ValueError):
    """A budget file problem; the message starts with path:line."""


class Budget:
    def __init__(self, figure: str, kind: str, measure: str, expected: str, tol: str, unit: str, source: str,
                 note: str, where: str = "budget"):
        self.figure, self.kind, self.measure = figure, kind, measure
        self.unit, self.source, self.note = unit, source, note
        self.limit: Optional[str] = None  # "<=", ">=", "<" or ">": one-sided
        self.expected: Optional[float] = None
        self.tol_text, self.tol = tol, 0.0
        m = None if expected == "-" else EXPECTED.match(expected)
        if expected != "-" and m is None:
            raise BudgetError("%s: expected '%s' is not a number, <=N, >=N, <N, >N or -" % (where, expected))
        if m:
            self.limit, self.expected = m.group(1), float(m.group(2))
        self._check_tolerance(where)
        self._check_kind(where)

    def _check_tolerance(self, where: str) -> None:
        if self.tol_text == "-":
            return
        t = TOLERANCE.match(self.tol_text)
        if t is None:
            raise BudgetError("%s: tolerance '%s' is not a number, a percentage or -" % (where, self.tol_text))
        if self.limit or self.expected is None:
            raise BudgetError("%s: a one-sided or unplanned figure takes tolerance '-'" % where)
        self.tol = float(t.group(1)) * (abs(self.expected) / 100.0 if t.group(2) else 1.0)

    def _check_kind(self, where: str) -> None:
        if self.kind == "unplanned" and self.expected is not None:
            raise BudgetError("%s: an unplanned figure carries no expected value" % where)
        if self.kind == "log":
            try:
                groups = re.compile(self.measure).groups
            except re.error as e:
                raise BudgetError("%s: log regex does not compile: %s" % (where, e))
            if groups != 1:
                raise BudgetError("%s: log regex needs exactly one group, has %d" % (where, groups))


def load_budgets(path: str) -> List[Budget]:
    out, names = [], set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, raw in enumerate(f, 1):
            cols = COMMENT.split(raw, 1)[0].split(None, 7)
            if not cols:
                continue
            where = "%s:%d" % (path, n)
            if len(cols) < 7 or cols[1] not in KINDS:
                raise BudgetError("%s: want '%s'" % (where, FORMAT))
            if cols[0] in names:
                raise BudgetError("%s: duplicate figure '%s'" % (where, cols[0]))
            names.add(cols[0])
            cols += [""] * (8 - len(cols))
            out.append(Budget(*cols, where=where))
    return out


def scan_log(lines, budgets: Sequence[Budget]) -> Dict[str, List[float]]:
    """One pass over the serial log's lines (a file object, never read whole): every log budget's numbers."""
    rx = [(b.figure, re.compile(b.measure)) for b in budgets if b.kind == "log"]
    found: Dict[str, List[float]] = {name: [] for name, _ in rx}
    for line in lines:
        for name, r in rx:
            m = r.search(line)
            if m:
                found[name].append(float(m.group(1)))
    return found


def measured(b: Budget, samples: List[soak_parse.Sample], log: Optional[Dict[str, List[float]]]) -> List[float]:
    """The numbers the run holds for a budget line (unknown -1 and absent keys left out)."""
    if b.kind == "log":
        return log.get(b.figure, []) if log is not None else []
    return [v for v in (soak_parse.value(s, b.measure) for s in samples) if v is not None]


def worst(b: Budget, nums: List[float]) -> float:
    if b.limit in ("<=", "<") or b.expected is None:
        return max(nums)
    if b.limit in (">=", ">"):
        return min(nums)
    return max(nums, key=lambda v: abs(v - b.expected))


def violates(b: Budget, v: float) -> bool:
    e = b.expected
    return {"<=": v > e, "<": v >= e, ">=": v < e, ">": v <= e}[b.limit]


def flag(b: Budget, nums: List[float]) -> str:
    if not nums:
        return "no data"
    if b.expected is None:
        return "unplanned"
    w = worst(b, nums)
    if b.limit:
        return "breach" if violates(b, w) else "ok"
    # Two-sided: judged on the median (a steady figure) and on the worst (a spike).
    for v in (statistics.median(nums), w):
        if v > b.expected + b.tol:
            return "over"
        if v < b.expected - b.tol:
            return "under"
    return "ok"


def fmt(v: Optional[float]) -> str:
    if v is None:
        return "-"
    return "%d" % v if float(v).is_integer() else "%.2f" % v


def expected_text(b: Budget) -> str:
    return "-" if b.expected is None else (b.limit or "") + fmt(b.expected)


def run(budgets: List[Budget], samples: List[soak_parse.Sample], log: Optional[Dict[str, List[float]]]) -> Tuple[list, bool]:
    """-> (rows, failed). A row is (figure, expected, tolerance, median, worst, flag, unit, how). Only usable
    (complete) samples count."""
    rows, failed = [], False
    good = soak_parse.usable(samples)
    for b in budgets:
        nums = measured(b, good, log)
        f = flag(b, nums)
        med = statistics.median(nums) if nums else None
        w = worst(b, nums) if nums else None
        rows.append((b.figure, expected_text(b), b.tol_text, fmt(med), fmt(w), f, b.unit, "%s:%s" % (b.kind, b.measure)))
        if f in ("over", "under", "breach") or (f == "no data" and b.kind == "key"):
            failed = True
    return rows, failed


def render(rows: list, samples: List[soak_parse.Sample], log_given: bool = True) -> str:
    head = ("figure", "expected", "tol", "median", "worst", "flag", "unit", "measured by")
    table = [head] + rows
    widths = [max(len(str(r[i])) for r in table) for i in range(len(head))]
    lines = ["  ".join(str(c).ljust(widths[i]) for i, c in enumerate(r)).rstrip() for r in table]
    reb = soak_parse.reboots(samples)
    bad = len(samples) - len(soak_parse.usable(samples))
    lines += ["", "samples: %d, incomplete (left out): %d, reboots inside the run: %d%s" % (
        len(samples), bad, len(reb), " (at samples %s)" % ", ".join(str(i) for i in reb) if reb else "")]
    if not log_given:
        lines.append("log figures: no data, no --log given")
    return "\n".join(lines)


def float_options(rest: List[str], spec: Dict[str, float]) -> Optional[str]:
    """Parse `--name value` pairs (numbers) into spec in place; -> the first unknown option, or None."""
    while rest:
        opt = rest.pop(0)
        if opt not in spec or not rest:
            return opt
        spec[opt] = float(rest.pop(0))
    return None


def run_census(args: List[str]) -> int:
    spec = {"--warmup-h": 2.0, "--window-h": 2.0, "--gap-min": 30.0, "--top": 40.0}
    bad = float_options(args[1:], spec) if args else "census <serial-log>"
    if bad is None and (spec["--window-h"] <= 0 or spec["--warmup-h"] < 0):
        bad = "--window-h must be positive"
    if bad is not None:
        print("soak_report: bad option " + bad, file=sys.stderr)
        return 2
    try:
        with open(args[0], encoding="utf-8", errors="replace") as f:
            c = soak_census.read_census(f, spec["--warmup-h"] * 3600, spec["--window-h"] * 3600, spec["--gap-min"] * 60)
    except OSError as e:
        print("soak_report: %s" % e, file=sys.stderr)
        return 2
    print("\n".join(soak_census.census_report(c, int(spec["--top"]))))
    return 0


def run_trend(args: List[str]) -> int:
    rest, allow_path = list(args[1:]), soak_trend.DEFAULT_ALLOW
    if "--allow" in rest:
        i = rest.index("--allow")
        allow_path = rest[i + 1] if i + 1 < len(rest) else ""
        del rest[i:i + 2]
    spec = {"--warmup-h": 2.0, "--interval-s": 600.0}
    bad = float_options(rest, spec) if args and allow_path else "trend <csv> [--allow FILE]"
    if bad is not None:
        print("soak_report: bad option " + bad, file=sys.stderr)
        return 2
    try:
        allow = soak_trend.load_allow(allow_path)
        samples = soak_parse.read_csv(args[0])
    except (OSError, ValueError) as e:
        print("soak_report: %s" % e, file=sys.stderr)
        return 2
    print("\n".join(soak_trend.trend_report(samples, allow, spec["--warmup-h"], spec["--interval-s"])))
    return 0


def main(argv: Sequence[str]) -> int:
    args = list(argv[1:])
    if args and args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    try:
        if len(args) >= 2 and args[0] == "census":
            return run_census(args[1:])
        if len(args) >= 2 and args[0] == "trend":
            return run_trend(args[1:])
    except ValueError as e:
        print("soak_report: %s" % e, file=sys.stderr)
        return 2
    if len(args) < 2 or args[0] != "expected":
        print(__doc__.split("\n")[0], file=sys.stderr)
        return 2
    csv_path, log_path, budgets_path = args[1], None, DEFAULT_BUDGETS
    rest = args[2:]
    while rest:
        opt = rest.pop(0)
        if opt == "--log" and rest:
            log_path = rest.pop(0)
        elif opt == "--budgets" and rest:
            budgets_path = rest.pop(0)
        else:
            print("soak_report: unknown option " + opt, file=sys.stderr)
            return 2
    try:
        budgets = load_budgets(budgets_path)
        samples = soak_parse.read_csv(csv_path)
        log = None
        if log_path:
            with open(log_path, encoding="utf-8", errors="replace") as f:
                log = scan_log(f, budgets)
    except (ValueError, OSError, re.error) as e:
        print("soak_report: %s" % e, file=sys.stderr)
        return 2
    rows, failed = run(budgets, samples, log)
    print(render(rows, samples, log_path is not None))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
