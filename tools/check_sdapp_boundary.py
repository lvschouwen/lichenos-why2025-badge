#!/usr/bin/env python3
"""Boundary check of the SD-card apps (#522): the L6 profile of tools/layers.conf applied to sdapps/<id>/ sources.
An SD app (a standalone ELF, no component) may include only its own headers, the public headers of lichen_api (never
lichen_sys_*.h, the OS's side) and the system headers `headers L6` allows; and it may not use a symbol `forbid L6`
names. Run through tools/check_app_boundary.sh; --root points at another tree (tools/tests/new_app_sd_test.sh)."""
import argparse
import fnmatch
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_layers as cl  # noqa: E402

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def sdapp_sources(root):
    base = os.path.join(root, "sdapps")
    for dp, dns, fns in os.walk(base):
        dns[:] = [d for d in dns if d not in ("build", "managed_components")]
        for fn in fns:
            if fn.endswith(cl.SOURCE_EXT):
                yield os.path.join(dp, fn)


def own_headers(root):
    return {os.path.basename(p) for p in sdapp_sources(root) if p.endswith((".h", ".hpp"))}


def api_headers(root):
    d = os.path.join(root, "firmware/components/lichen_api")
    return {fn for fn in os.listdir(d) if fn.endswith(".h") and not fn.startswith("lichen_sys_")}


def include_problem(conf, inc, own, api):
    base = os.path.basename(inc)
    if base in own or (base in api and inc == base):
        return None
    if base.startswith("lichen_sys_"):
        return "%s is the OS's side (lichen_sys_*), not an app header" % inc
    if any(fnmatch.fnmatchcase(inc, g) for g in conf.headers["L6"]):
        return None
    return "%s is neither the app's own, a public lichen_api header nor an allowed system header (headers L6)" % inc


def check(root):
    conf = cl.parse_conf(cl.read(os.path.join(root, cl.CONF_REL)))
    own, api = own_headers(root), api_headers(root)
    forbid = conf.forbid["L6"]
    problems = []
    for path in sorted(sdapp_sources(root)):
        rel = os.path.relpath(path, root)
        for n, inc in cl.includes_of(path):
            why = include_problem(conf, inc, own, api)
            if why:
                problems.append("%s:%d: %s" % (rel, n, why))
        with open(path, encoding="utf-8", errors="replace") as f:
            text = COMMENTS.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), f.read())
        for n, line in enumerate(text.splitlines(), 1):
            if re.match(r"\s*#\s*include", line):
                continue
            for sym in IDENT.findall(line):
                if any(fnmatch.fnmatchcase(sym, g) for g in forbid):
                    problems.append("%s:%d: %s is forbidden to apps (forbid L6)" % (rel, n, sym))
    return problems


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    args = ap.parse_args(argv)
    problems = check(os.path.abspath(args.root))
    for p in problems:
        print("sdapp boundary: " + p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
