#!/usr/bin/env python3
"""Summarise a v3 bench log (the UART output of a load-ram run) into a table and a verdict (#611).

    v3_bench_report.py <log> [--require ID ...] [--no-default-require]

The log is the protocol of v3/kcore/src/check.rs: a banner `lichen-v3 <build id> platform <p> ...`, lines
`CHECK <id> <PASS|FAIL|INFO> <detail>`, `BEGIN <id>` before risky steps, and a last line `END <reason>`. ROM and esptool
noise, CRLF and ANSI escapes are ignored. Exit codes (stable, for wrappers):
  0  END seen with reason ok, no FAIL, banner seen, every required id present and not FAIL
  1  anything else: no END (a hang or crash: the last BEGIN is named), END not ok, a FAIL, a missing required id
  2  the log is unreadable or usage is wrong
Default required ids: boot.hart image.layout c6.strap c6.en l2.cache l2.fit.
"""
import re
import sys
from dataclasses import dataclass, field

DEFAULT_REQUIRED = ["boot.hart", "image.layout", "c6.strap", "c6.en", "l2.cache", "l2.fit"]
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
BANNER = re.compile(r"lichen-v3 (\S+) platform (\S+)")
CHECK = re.compile(r"^CHECK (\S+) (PASS|FAIL|INFO)(?: (.*))?$")
BEGIN = re.compile(r"^BEGIN (\S+)")
END = re.compile(r"^END(?: (.*))?$")


@dataclass
class Report:
    build_id: str = ""
    platform: str = ""
    checks: list = field(default_factory=list)  # (id, verdict, detail)
    last_begin: str = ""
    began_unfinished: bool = False
    end: "str | None" = None


def parse(text: str) -> Report:
    r = Report()
    for raw in text.replace("\r", "\n").split("\n"):
        line = ANSI.sub("", raw).strip()
        if m := BANNER.search(line):
            r.build_id, r.platform = m.group(1), m.group(2)
        elif m := CHECK.match(line):
            r.checks.append((m.group(1), m.group(2), m.group(3) or ""))
        elif m := BEGIN.match(line):
            r.last_begin = m.group(1)
        elif m := END.match(line):
            if r.end is None:  # the first END is the run's; later text is the host resetting into v2
                r.end = (m.group(1) or "").strip()
    return r


def verdict(r: Report, required: list) -> list:
    """Problems, empty when the run is clean."""
    problems = []
    if not r.build_id:
        problems.append("no banner")
    if r.end is None:
        hint = f" (last BEGIN: {r.last_begin})" if r.last_begin else ""
        problems.append(f"no END: the run hung or crashed{hint}")
    elif r.end != "ok":
        problems.append(f"END {r.end}")
    seen = {}
    for cid, v, _ in r.checks:
        seen[cid] = "FAIL" if seen.get(cid) == "FAIL" or v == "FAIL" else v
    for cid, v, detail in r.checks:
        if v == "FAIL":
            problems.append(f"FAIL {cid}: {detail}")
    for cid in required:
        if cid not in seen:
            problems.append(f"missing {cid}")
    return problems


def render(r: Report, problems: list) -> str:
    out = [f"build {r.build_id or '?'}  platform {r.platform or '?'}  END {r.end if r.end is not None else '(none)'}"]
    width = max([len(c[0]) for c in r.checks] + [2])
    for cid, v, detail in r.checks:
        out.append(f"  {cid:<{width}}  {v}  {detail}")
    out.append("RESULT: " + ("PASS" if not problems else "FAIL"))
    out.extend(f"  - {p}" for p in problems)
    return "\n".join(out)


def main(argv: list) -> int:
    args = argv[1:]
    required, use_default, path = [], True, None
    i = 0
    while i < len(args):
        if args[i] == "--require" and i + 1 < len(args):
            required.append(args[i + 1])
            i += 2
        elif args[i] == "--no-default-require":
            use_default = False
            i += 1
        elif path is None and not args[i].startswith("--"):
            path = args[i]
            i += 1
        else:
            print(__doc__, file=sys.stderr)
            return 2
    if path is None:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError as e:
        print(f"v3_bench_report: {e}", file=sys.stderr)
        return 2
    r = parse(text)
    problems = verdict(r, (DEFAULT_REQUIRED if use_default else []) + required)
    print(render(r, problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
