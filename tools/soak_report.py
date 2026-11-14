#!/usr/bin/env python3
"""Soak report (#424, #407): `soak_report.py expected <samples.csv> [--log <serial.log>] [--budgets FILE]`.
Prints the expected-against-measured table of tools/soak_budgets.txt (format documented there): key, expected,
tolerance, measured median, measured worst, flag. Exit 0 when nothing is flagged, 1 otherwise.
Flags: ok, over, under (outside the tolerance, both directions), unplanned (measured only, no plan), no data.
`over`, `under` and a `no data` on a soak-key figure fail the run (a planned key the CSV lacks is a broken sampler);
`unplanned` and `no data` on a probe or log figure are shown but do not. Worst = the value furthest past the
expectation (min for `>=`, max for `<=`, farthest from expected for a tolerance, max for unplanned).
Standard library only."""
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_parse  # noqa: E402

DEFAULT_BUDGETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "soak_budgets.txt")
# A comment is a "#" followed by a space or the line end: "#234" (an issue) is a source, not a comment.
COMMENT = re.compile(r"(?:^|\s)#(?:\s|$)")
KINDS = ("key", "log", "probe", "unplanned")


class Budget:
    def __init__(self, figure, kind, measure, expected, tol, unit, source, note):
        self.figure, self.kind, self.measure = figure, kind, measure
        self.unit, self.source, self.note = unit, source, note
        self.limit = None  # "<=" or ">="
        self.expected = None
        if expected[:2] in ("<=", ">="):
            self.limit, self.expected = expected[:2], float(expected[2:])
        elif expected != "-":
            self.expected = float(expected)
        self.tol_text = tol
        self.tol = 0.0
        if tol.endswith("%"):
            self.tol = abs(self.expected or 0.0) * float(tol[:-1]) / 100.0
        elif tol != "-":
            self.tol = float(tol)


def load_budgets(path):
    out = []
    with open(path) as f:
        for n, raw in enumerate(f, 1):
            cols = COMMENT.split(raw, 1)[0].split(None, 7)
            if not cols:
                continue
            if len(cols) < 7 or cols[1] not in KINDS:
                raise ValueError("%s:%d: want 'figure kind measure expected tolerance unit source [note]'" % (path, n))
            cols += [""] * (8 - len(cols))
            out.append(Budget(*cols))
    return out


def log_numbers(log_text, pattern):
    rx = re.compile(pattern)
    nums = []
    for line in log_text.splitlines():
        m = rx.search(line)
        if m:
            nums.append(float(m.group(1)))
    return nums


def measured(b, samples, log_text):
    """The numbers the run holds for a budget line (unknown -1 and absent keys left out)."""
    if b.kind in ("key", "unplanned") or b.kind == "probe":
        return [v for v in (soak_parse.value(s, b.measure) for s in samples) if v is not None]
    return log_numbers(log_text, b.measure) if log_text is not None else []


def worst(b, nums):
    if b.limit == "<=" or b.expected is None:
        return max(nums)
    if b.limit == ">=":
        return min(nums)
    return max(nums, key=lambda v: abs(v - b.expected))


def flag(b, nums):
    if not nums:
        return "no data"
    if b.expected is None:
        return "unplanned"
    med, w = statistics.median(nums), worst(b, nums)
    if b.limit == "<=":
        return "over" if w > b.expected else "ok"
    if b.limit == ">=":
        return "over" if w < b.expected else "ok"
    # Two-sided: judged on the median (a steady figure) and on the worst (a spike); the side of the worst names it.
    for v in (med, w):
        if v > b.expected + b.tol:
            return "over"
        if v < b.expected - b.tol:
            return "under"
    return "ok"


def fmt(v):
    if v is None:
        return "-"
    return "%d" % v if float(v).is_integer() else "%.2f" % v


def expected_text(b):
    return "-" if b.expected is None else (b.limit or "") + fmt(b.expected)


def run(budgets, samples, log_text):
    """-> (rows, failed). A row is (figure, expected, tolerance, median, worst, flag, unit, how)."""
    rows, failed = [], False
    for b in budgets:
        nums = measured(b, samples, log_text)
        f = flag(b, nums)
        med = statistics.median(nums) if nums else None
        w = worst(b, nums) if nums else None
        rows.append((b.figure, expected_text(b), b.tol_text, fmt(med), fmt(w), f, b.unit, "%s:%s" % (b.kind, b.measure)))
        if f in ("over", "under") or (f == "no data" and b.kind == "key"):
            failed = True
    return rows, failed


def render(rows, samples):
    head = ("figure", "expected", "tol", "median", "worst", "flag", "unit", "measured by")
    table = [head] + rows
    widths = [max(len(str(r[i])) for r in table) for i in range(len(head))]
    lines = ["  ".join(str(c).ljust(widths[i]) for i, c in enumerate(r)).rstrip() for r in table]
    reb = soak_parse.reboots(samples)
    lines.append("")
    lines.append("samples: %d, reboots inside the run: %d%s" % (len(samples), len(reb),
                 " (at samples %s)" % ", ".join(str(i) for i in reb) if reb else ""))
    return "\n".join(lines)


def main(argv):
    args = list(argv[1:])
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
    samples = soak_parse.read_csv(csv_path)
    log_text = open(log_path, errors="replace").read() if log_path else None
    rows, failed = run(load_budgets(budgets_path), samples, log_text)
    print(render(rows, samples))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
