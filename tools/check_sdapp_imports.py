#!/usr/bin/env python3
"""Import gate of the SD-card apps (#550): every symbol an app's ELF leaves undefined must be in tools/export_table.txt,
or the loader refuses the app on the badge ("Can't find common <symbol>") and nothing at build time said so. The ELFs
are stripped, so the undefined symbols come from the dynamic symbol table (the symbols their relocations name).
Usage: check_sdapp_imports.py [--table FILE] [--readelf PROG] app.elf... ; exit 1 and the app and symbol named on a miss."""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def exported_names(table_text):
    """The names an app may import: column 2 of the `export` lines."""
    names = set()
    for line in table_text.splitlines():
        cols = line.split("\t")
        if len(cols) >= 3 and cols[0] == "export":
            names.add(cols[1])
    return names


def undefined_symbols(readelf_text):
    """The named undefined symbols of `readelf -W --dyn-syms` output (the index-0 null symbol has no name)."""
    out = []
    for line in readelf_text.splitlines():
        cols = line.split()
        if len(cols) >= 8 and cols[0].endswith(":") and cols[6] == "UND":
            out.append(cols[7].split("@")[0])
    return out


def missing_imports(readelf_text, table_text):
    allowed = exported_names(table_text)
    return sorted({s for s in undefined_symbols(readelf_text) if s not in allowed})


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--table", default=os.path.join(HERE, "export_table.txt"))
    ap.add_argument("--readelf", default=None)
    ap.add_argument("elfs", nargs="+")
    args = ap.parse_args(argv)
    readelf = args.readelf or shutil.which("riscv32-esp-elf-readelf")
    if not readelf:
        print("check_sdapp_imports: riscv32-esp-elf-readelf is not on PATH (export the ESP-IDF environment)", file=sys.stderr)
        return 2
    with open(args.table, encoding="utf-8") as f:
        table = f.read()
    bad = 0
    for elf in args.elfs:
        res = subprocess.run([readelf, "-W", "--dyn-syms", elf], capture_output=True, text=True, check=False)
        if res.returncode != 0 or not undefined_symbols(res.stdout):
            print(f"check_sdapp_imports: {elf}: no dynamic symbols read ({res.stderr.strip()[:200]})", file=sys.stderr)
            bad += 1
            continue
        app = os.path.basename(os.path.dirname(elf))
        for sym in missing_imports(res.stdout, table):
            print(f"sdapps/{app}: imports {sym}, which tools/export_table.txt does not export (the loader would refuse it)", file=sys.stderr)
            bad += 1
    if not bad:
        print(f"sdapp imports: {len(args.elfs)} app(s), every import is in the export table")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
