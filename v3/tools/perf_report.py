#!/usr/bin/env python3
"""Lichen v3 perf report (#643): parse PERF lines from a load-ram log, compare with v2 and the targets, ratchet.

PERF grammar: `PERF <metric.dotted> key=value ...`, values are plain numbers, the unit lives in the key name.
Exit 0 ok, 1 a ratcheted metric is worse than its recorded best beyond its tolerance, 2 bad input.
"""
import argparse
import json
import math
import re
import sys
import tomllib
from pathlib import Path

PERF_DIR = Path(__file__).resolve().parent.parent / "perf"
LINE = re.compile(r"^\s*(?:\[[^\]]*\]\s*)?PERF\s+([A-Za-z0-9_.]+)((?:\s+[A-Za-z0-9_]+=\S+)*)\s*$")
NUM = re.compile(r"^-?\d+(?:\.\d+)?$")
DEFAULT_TOL = 5.0


def parse_log(text):
    """Return ({metric: {key: float}}, [warnings]). A later line for the same metric replaces an earlier one."""
    out, warn = {}, []
    for n, raw in enumerate(text.splitlines(), 1):
        if "PERF " not in raw:
            continue
        m = LINE.match(raw.rstrip("\r"))
        if not m:
            warn.append(f"line {n}: not a PERF line: {raw.strip()[:60]}")
            continue
        vals = {}
        for pair in m.group(2).split():
            k, v = pair.split("=", 1)
            if NUM.match(v):
                vals[k] = float(v)
            else:
                warn.append(f"line {n}: {m.group(1)} {k}={v} is not a number")
        if vals:
            out[m.group(1)] = vals
    return out, warn


def worse(direction, a, b):
    """True when a is worse than b."""
    return a > b if direction == "lower" else a < b


def beyond_tolerance(direction, now, best, tol_pct):
    """True when `now` is worse than `best` by more than tol_pct of best."""
    slack = abs(best) * tol_pct / 100.0
    return worse(direction, now, best) and abs(now - best) > slack + 1e-12


def target_of(t):
    """Absolute target, or computed from v2 and target_pct, or None."""
    if "target" in t:
        return float(t["target"])
    v2, pct = t.get("v2"), t.get("target_pct")
    if isinstance(v2, (int, float)) and pct is not None:
        return v2 * (1 + pct / 100.0)
    return None


def status_of(t, value):
    if value is None:
        return "-"
    tgt, d = target_of(t), t["direction"]
    if tgt is None:
        return "no-target"
    if "stretch" in t and not worse(d, value, float(t["stretch"])):
        return "stretch"
    return "ok" if not worse(d, value, tgt) else "miss"


def evaluate(targets, best, measured, build, tol_override=None):
    """Build per-metric rows and the list of regressions. Unmeasured metrics are skipped, never failed."""
    rows, regress = [], []
    for name, t in targets.items():
        got = measured.get(name, {}).get(t["key"])
        row = {"metric": name, "key": t["key"], "v2": t.get("v2", "todo"), "v3": got, "target": target_of(t),
               "delta_pct": None, "status": status_of(t, got), "commit": build if got is not None else "-"}
        v2 = row["v2"]
        if got is not None and isinstance(v2, (int, float)) and v2 != 0:
            row["delta_pct"] = (got - v2) / abs(v2) * 100.0
        b = best.get(name, {}).get("value")
        if got is not None and t.get("ratchet") and b is not None:
            tol = tol_override if tol_override is not None else float(t.get("tolerance_pct", DEFAULT_TOL))
            if beyond_tolerance(t["direction"], got, b, tol):
                row["status"] = "REGRESS"
                regress.append(f"{name}: {got:g} is worse than best {b:g} (tolerance {tol:g}%)")
        rows.append(row)
    return rows, regress


def fmt(v, nd=2):
    if v is None:
        return "-"
    if isinstance(v, str):
        return v
    return f"{v:.{nd}f}".rstrip("0").rstrip(".") if not float(v).is_integer() else str(int(v))


def table(rows):
    head = ["metric", "v2", "v3", "target", "d% vs v2", "status", "commit"]
    body = [[r["metric"] + "." + r["key"], fmt(r["v2"]), fmt(r["v3"]), fmt(r["target"]),
             "-" if r["delta_pct"] is None else f"{r['delta_pct']:+.1f}", r["status"], r["commit"]] for r in rows]
    w = [max(len(x[i]) for x in [head] + body) for i in range(len(head))]
    line = lambda c: "  ".join(s.ljust(w[i]) for i, s in enumerate(c)).rstrip()
    return "\n".join([line(head), line(["-" * n for n in w])] + [line(b) for b in body])


def render_best(best):
    lines = ["# Best measured value per ratcheted metric (#643). Only `perf_report.py --update-best` writes it, and only",
             "# when a value improves. Entry shape: [best.\"name\"] value = <number>, build = \"<id>\"."]
    for name in sorted(best):
        lines += ["", f'[best."{name}"]', f"value = {best[name]['value']!r}", f'build = "{best[name]["build"]}"']
    return "\n".join(lines) + "\n"


def improve_best(targets, best, measured, build):
    """Return the new best dict; a ratcheted value replaces best only when strictly better."""
    new = {k: dict(v) for k, v in best.items()}
    for name, t in targets.items():
        got = measured.get(name, {}).get(t["key"])
        if got is None or not t.get("ratchet") or not math.isfinite(got):
            continue
        cur = new.get(name, {}).get("value")
        if cur is None or worse(t["direction"], cur, got):
            new[name] = {"value": got, "build": build}
    return new


def load_toml(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--log", required=True, help="load-ram log containing PERF lines")
    ap.add_argument("--build", required=True, help="build id, names perf/results/<id>.json")
    ap.add_argument("--update-best", action="store_true", help="record improved ratcheted values in best.toml")
    ap.add_argument("--perf-dir", default=str(PERF_DIR), help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9._-]+", a.build):
        print("bad --build id", file=sys.stderr)
        return 2
    pdir = Path(a.perf_dir)
    targets = load_toml(pdir / "targets.toml")
    best = load_toml(pdir / "best.toml").get("best", {})
    measured, warn = parse_log(Path(a.log).read_text(errors="replace"))
    for w in warn:
        print("warning:", w, file=sys.stderr)
    rows, regress = evaluate(targets, best, measured, a.build)
    (pdir / "results").mkdir(exist_ok=True)
    (pdir / "results" / f"{a.build}.json").write_text(
        json.dumps({"build": a.build, "metrics": measured}, indent=2, sort_keys=True) + "\n")
    print(table(rows))
    unknown = sorted(set(measured) - set(targets))
    if unknown:
        print("not in targets.toml (ignored):", ", ".join(unknown))
    if a.update_best and not regress:
        (pdir / "best.toml").write_text(render_best(improve_best(targets, best, measured, a.build)))
    for r in regress:
        print("REGRESSION:", r, file=sys.stderr)
    return 1 if regress else 0


if __name__ == "__main__":
    sys.exit(main())
