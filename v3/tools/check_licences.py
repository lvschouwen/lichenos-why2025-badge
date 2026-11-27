#!/usr/bin/env python3
"""Licence check for v3 (#611): every package in the Cargo workspace graph, dev-dependencies included, must be
MIT or Apache-2.0 by its SPDX `license` field. `A OR B` passes when either side does, `A AND B` when both do,
`X WITH Y` is judged by X. A package without a license field fails. A miss is an owner decision, never an exception
added here.

    check_licences.py            reads `cargo metadata --locked` of v3/
    check_licences.py --stdin    reads that JSON from stdin (tests)
Exit 0 = clean, 1 = findings, 2 = metadata unreadable.
"""
import json
import re
import subprocess
import sys

ALLOWED = {"MIT", "Apache-2.0"}
TOKEN = re.compile(r"\(|\)|[^\s()]+")


def allowed(expr: str) -> bool:
    toks = TOKEN.findall(expr.replace("/", " OR "))
    pos = 0

    def peek():
        return toks[pos] if pos < len(toks) else None

    def take():
        nonlocal pos
        pos += 1
        return toks[pos - 1]

    def atom() -> bool:
        t = take()
        if t == "(":
            v = parse_or()
            if peek() != ")":
                raise ValueError(f"unbalanced parentheses in {expr!r}")
            take()
            return v
        v = t in ALLOWED
        if peek() == "WITH":
            take()
            take()
        return v

    def parse_and() -> bool:
        v = atom()
        while peek() == "AND":
            take()
            v = atom() and v
        return v

    def parse_or() -> bool:
        v = parse_and()
        while peek() == "OR":
            take()
            v = parse_and() or v
        return v

    if not toks:
        return False
    v = parse_or()
    if pos != len(toks):
        raise ValueError(f"cannot parse licence expression {expr!r}")
    return v


def findings(meta: dict) -> list:
    out = []
    for p in meta["packages"]:
        lic = p.get("license")
        if not lic:
            out.append(f"{p['name']} {p['version']}: no license field")
            continue
        try:
            ok = allowed(lic)
        except (ValueError, IndexError):
            ok = False
        if not ok:
            out.append(f"{p['name']} {p['version']}: {lic}")
    return out


def main(argv: list) -> int:
    try:
        if argv[1:] == ["--stdin"]:
            meta = json.load(sys.stdin)
        else:
            raw = subprocess.run(["cargo", "metadata", "--locked", "--format-version", "1"], check=True,
                                 capture_output=True, text=True).stdout
            meta = json.loads(raw)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as e:
        print(f"check_licences: {e}", file=sys.stderr)
        return 2
    bad = findings(meta)
    for b in bad:
        print(f"check_licences: not MIT/Apache-2.0: {b}")
    if not bad:
        print(f"check_licences: {len(meta['packages'])} packages, all MIT or Apache-2.0")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
