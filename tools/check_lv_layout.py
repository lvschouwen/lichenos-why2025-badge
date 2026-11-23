#!/usr/bin/env python3
"""LVGL layout check of the SD apps (#522): compiles tools/lv_layout_probe.c with the firmware's flags and with an SD
app's flags (both taken from their compile_commands.json) to assembly and compares the sizeof/offsetof values it emits.
A difference means the app's LVGL configuration is not the firmware's. build.sh runs it after each SD app build; the
fixtures of tools/tests/check_lv_layout_test.py use --probe with a stand-in probe."""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys

DEFAULT_PROBE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lv_layout_probe.c")
DROP_WITH_ARG = ("-o", "-c", "-MF", "-MT", "-MQ")
DROP = ("-MD", "-MMD", "-fstack-usage", "-save-temps")
WORD = re.compile(r"^\s*\.(?:word|4byte|long)\s+(\d+)\s*$")


class CheckError(Exception):
    pass


def pick(db_path, want):
    with open(db_path, encoding="utf-8") as f:
        for e in json.load(f):
            if want(e["file"]):
                return e
    raise CheckError("%s: no matching compile command" % db_path)


def probe_command(entry, probe):
    toks = shlex.split(entry["command"])
    out, skip = [], False
    for t in toks:
        if skip:
            skip = False
        elif t in DROP_WITH_ARG:
            skip = True
        elif t in DROP or t == entry["file"]:
            continue
        else:
            out.append(t)
    return out + ["-S", "-o", "-", probe]


def layout(entry, probe):
    r = subprocess.run(probe_command(entry, probe), cwd=entry["directory"], capture_output=True, text=True)
    if r.returncode:
        raise CheckError("probe does not compile with the flags of %s:\n%s" % (entry["file"], r.stderr[-2000:]))
    vals, on = [], False
    for line in r.stdout.splitlines():
        if line.startswith("lv_layout:"):
            on = True
        elif on:
            m = WORD.match(line)
            if m:
                vals.append(int(m.group(1)))
            elif line.lstrip().startswith(".size") or (line and not line[0].isspace() and line.endswith(":")):
                break
    if not vals:
        raise CheckError("no lv_layout values in the assembly of %s" % entry["file"])
    return vals


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fw", required=True, help="firmware compile_commands.json")
    ap.add_argument("--app", required=True, help="the SD app's compile_commands.json")
    ap.add_argument("--fw-file", default="lichen_ui.c", help="basename of the firmware source whose flags are used")
    ap.add_argument("--probe", default=DEFAULT_PROBE)
    args = ap.parse_args(argv)
    try:
        fw = layout(pick(args.fw, lambda f: os.path.basename(f) == args.fw_file), args.probe)
        app = layout(pick(args.app, lambda f: f.endswith(".c") and "/main/" in f), args.probe)
    except CheckError as e:
        print("lv layout: %s" % e, file=sys.stderr)
        return 1
    if fw != app:
        bad = [i for i, (a, b) in enumerate(zip(fw, app)) if a != b] or [min(len(fw), len(app))]
        print("lv layout: the SD app's LVGL structs differ from the firmware's at probe entries %s "
              "(firmware %s, app %s): sdapps/sdapp.cmake does not reproduce the firmware's LVGL config"
              % (bad, fw, app), file=sys.stderr)
        return 1
    print("lv layout: %d sizes and offsets match the firmware" % len(fw))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
