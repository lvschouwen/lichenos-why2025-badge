#!/usr/bin/env python3
"""Export table of the SD-card apps (#519, epic #516). Run with python3 -I.

An SD app is an ELF whose undefined symbols the OS resolves by name when it loads it. The names it may resolve are this
table, opt-in: the headers and functions tools/export_api.txt lists (taken from the same header walk as
tools/gate_manifest.txt, minus what `forbid L6` in tools/layers.conf bars an app from), plus tools/export_allow.txt (a
small libc set, the malloc family mapped to the OS's shims, the lv_* the built-in apps call).

  --write   regenerate firmware/components/lichen_os/lichen_exports.c and tools/export_table.txt
  --check   fail when either differs from what the headers give today, when an exported function is wrapped without a
            mapping, when the table changed without a LICHEN_API_VERSION bump (against the merge base with main) and,
            with --elf, when an app's undefined symbols are not in the table

tools/export_table.txt is the reviewable record: `export<TAB>name<TAB>symbol<TAB>origin<TAB>hash` (hash: of the normalised prototype), one `wrap` line per
-Wl,--wrap=X found in a component's CMakeLists.txt (a new wrap shows up as a diff), and the API version it was made at."""
import argparse
import fnmatch
import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_layers as cl  # noqa: E402
from export_proto import declarations_hash, header_closure, prototype_hash  # noqa: E402,F401

ALLOW_REL = "tools/export_allow.txt"
API_REL = "tools/export_api.txt"
TABLE_REL = "tools/export_table.txt"
C_REL = "firmware/components/lichen_os/lichen_exports.c"
VERSION_REL = "firmware/components/lichen_api/lichen_api_version.h"
WRAP_RE = re.compile(r"--wrap=(\w+)")
NAME_RE = re.compile(r"^[A-Za-z_]\w*$")


class ExportError(Exception):
    pass


def parse_allow(text):
    """[(name, target or None)]: `name` or `name = target` (the app's name maps to the OS's own function)."""
    out = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split("=")]
        if len(parts) > 2 or not all(NAME_RE.match(p) for p in parts):
            raise ExportError("%s line %d: expected `name` or `name = target`" % (ALLOW_REL, n))
        out.append((parts[0], parts[1] if len(parts) == 2 else None))
    return out


def parse_api(text):
    """(headers, funcs, denies) of tools/export_api.txt, each in file order."""
    got = {"header": [], "func": [], "deny": []}
    for n, raw in enumerate(text.splitlines(), 1):
        f = raw.split("#", 1)[0].split()
        if not f:
            continue
        if len(f) != 2 or f[0] not in got or not re.match(r"^[\w.]+$", f[1]):
            raise ExportError("%s line %d: expected `header H.h`, `func NAME` or `deny NAME`" % (API_REL, n))
        got[f[0]].append(f[1])
    return got["header"], got["func"], got["deny"]


def walked_dirs(root, conf):
    """The directories whose headers an app may include: the gate's and the peer components' (surface_functions' walk)."""
    dirs = [cl.gate_dir(root)]
    comps = cl.repo_components(root)
    for (comp, peer), _globs in sorted(conf.peer_headers.items()):
        if conf.comp_layer.get(comp) in conf.closed and peer in comps and comps[peer] not in dirs:
            dirs.append(comps[peer])
    return dirs


def header_prototypes(root, conf):
    """{function: (header, prototype hash)} over the headers the surface walk covers. A function declared in two of them
    fails: the table would have to pick one, and a hash of the other would go unchecked."""
    out = {}
    surface = set((f, h) for f, h, _ in cl.surface_functions(root, conf))  # only the headers apps may include
    for d in walked_dirs(root, conf):
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".h"):
                continue
            for stmt, _block in cl.scan_statements(cl.read(os.path.join(d, fn))):
                name = cl.statement_function(stmt)
                if not name or (name, fn) not in surface:
                    continue
                if name in out and out[name][0] != fn:
                    raise ExportError("%s is declared in both %s and %s: keep one declaration" % (name, out[name][0], fn))
                out.setdefault(name, (fn, prototype_hash(stmt)))
    return out


def type_hashes(root, conf, listed):
    """{header: hash of its non-function declarations} for the listed headers and the headers they include from their own
    directory (lichen_caps.h under lichen_app.h): a changed typedef, struct, enum or macro breaks apps with no prototype
    changing."""
    paths = {}
    for d in walked_dirs(root, conf):
        for fn in os.listdir(d):
            if fn.endswith(".h"):
                paths.setdefault(fn, os.path.join(d, fn))
    seen = {}
    for h in sorted(listed):
        header_closure(paths[h], seen)
    return {h: declarations_hash(cl.read(p)) for h, p in sorted(seen.items())}


def find_wraps(root):
    """Every X of -Wl,--wrap=X in the P4 components' CMakeLists.txt (the C6 image has its own libc and no SD apps)."""
    base = os.path.join(root, "firmware", "components")
    found = set()
    for d, dirs, files in os.walk(base):
        dirs[:] = [x for x in dirs if x not in ("build", "managed_components")]
        if "CMakeLists.txt" in files:
            for line in cl.read(os.path.join(d, "CMakeLists.txt")).splitlines():
                found.update(WRAP_RE.findall(line.split("#", 1)[0]))  # a commented-out wrap is no wrap
    return sorted(found)


def api_version(root):
    text = cl.read(os.path.join(root, VERSION_REL))
    nums = []
    for k in ("MAJOR", "MINOR"):
        m = re.search(r"#\s*define\s+LICHEN_API_VERSION_%s\s+(\d+)" % k, text)
        if m is None:
            raise ExportError("%s must define LICHEN_API_VERSION_%s" % (VERSION_REL, k))
        nums.append(int(m.group(1)))
    return nums[0], nums[1]


def forbidden(conf, name):
    return any(fnmatch.fnmatchcase(name, g) for g in conf.forbid.get("L6", []))


def api_selection(root, conf, protos):
    """{function: (header, hash)} the curated list names; a line that matches nothing is an error."""
    headers, funcs, denies = parse_api(cl.read(os.path.join(root, API_REL)))
    sel = {}
    for h in headers:
        names = [n for n, (hh, _) in protos.items() if hh == h]
        if not names:
            raise ExportError("%s: header %s declares no function the gate walk sees" % (API_REL, h))
        sel.update((n, protos[n]) for n in names if not forbidden(conf, n))
    for d in denies:
        if d not in sel:
            raise ExportError("%s: deny %s matches nothing in the listed headers (or layers.conf already forbids it)" % (API_REL, d))
        del sel[d]
    for f in funcs:
        if f not in protos:
            raise ExportError("%s: func %s is declared in no gate or peer header" % (API_REL, f))
        if forbidden(conf, f):
            raise ExportError("%s: func %s is forbidden to apps (layers.conf)" % (API_REL, f))
        if f in sel:
            raise ExportError("%s: %s is already exported by a listed header" % (API_REL, f))
        sel[f] = protos[f]
    return sel


def build_exports(root, conf):
    """([(name, symbol, origin, hash)] sorted by name, {header: type hash}); symbol is what the table points at (a wrapped
    function or a shim). The libc, libm and lv_* rows hash their target symbol, so a retarget shows in the table."""
    wraps = set(find_wraps(root))
    sel = api_selection(root, conf, header_prototypes(root, conf))
    rows = {n: (n, h, ph) for n, (h, ph) in sel.items()}
    for name, target in parse_allow(cl.read(os.path.join(root, ALLOW_REL))):
        if name in rows:
            raise ExportError("%s: %s is already exported from %s" % (ALLOW_REL, name, rows[name][1]))
        if target is None and forbidden(conf, name):
            raise ExportError("%s: %s is forbidden to apps (layers.conf); map it to an OS function or drop it" % (ALLOW_REL, name))
        rows[name] = (target or name, "allow", hashlib.sha256((target or name).encode()).hexdigest()[:8])
    out = []
    for name in sorted(rows):
        sym, origin, ph = rows[name]
        if sym in wraps:
            sym = "__wrap_" + sym  # the linker would redirect &sym there anyway; the table names it so the diff shows it
        out.append((name, sym, origin, ph))
    return out, type_hashes(root, conf, set(h for _, h, _ in rows.values() if h != "allow"))


def table_text(exports, wraps, version, hdrs):
    head = (
        "# Export table of the SD-card apps (#519): what an app's ELF may resolve against the OS. Generated by\n"
        "# `tools/gen_export_table.py --write` from tools/export_api.txt (the curated app API), tools/export_allow.txt and the\n"
        "# forbid lines of tools/layers.conf; checked on every build. Changing it needs a LICHEN_API_VERSION bump: major\n"
        "# when a line is removed or its prototype hash changes, minor when one is added (lichen_api_version.h).\n"
        "# export<TAB>name<TAB>symbol<TAB>origin (header, or allow)<TAB>hash (normalised prototype; the target symbol's for\n"
        "# allow rows); header<TAB>H.h<TAB>hash: the types, structs, enums and macros of every header the exports come from and\n"
        "# what it includes (a change is a major bump);\n"
        "# wrap<TAB>X<TAB>__wrap_X: every -Wl,--wrap=X (informational: a new wrap needs no bump).\n"
    )
    body = "api\t%d.%d\n" % version
    body += "".join("wrap\t%s\t__wrap_%s\n" % (w, w) for w in wraps)
    body += "".join("header\t%s\t%s\n" % h for h in sorted(hdrs.items()))
    body += "".join("export\t%s\t%s\t%s\t%s\n" % e for e in exports)
    return head + body


def c_text(exports):
    out = ["// Generated by tools/gen_export_table.py (#519), do not edit: the names an SD app's ELF may resolve (tools/export_table.txt).\n",
           '#include "lichen_exports.h"\n\n',
           "// Prototypes are placeholders: only the address is taken. The libc names are builtins to the compiler.\n",
           '#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"\n',
           '#pragma GCC diagnostic ignored "-Wattributes"\n\n']
    out += ["extern void %s(void);\n" % s for s in sorted(set(e[1] for e in exports))]
    out.append("\nstruct esp_elfsym const lichen_exports[] = {\n")
    out += ['    {"%s", (void *)%s},\n' % (n, s) for n, s, _, _ in exports]
    out.append("    {NULL, NULL},\n};\n")
    return "".join(out)


def parse_table(text):
    """(version tuple or None, {name: hash}, wraps, {header: type hash}) of an export_table.txt."""
    version, exports, wraps, hdrs = None, {}, [], {}
    for raw in text.splitlines():
        f = raw.split("\t")
        if f[0] == "api":
            version = tuple(int(x) for x in f[1].split("."))
        elif f[0] == "export":
            exports[f[1]] = f[4] if len(f) > 4 else f[2]  # the prototype hash; a pre-hash table compares by symbol
        elif f[0] == "wrap":
            wraps.append(f[1])
        elif f[0] == "header":
            hdrs[f[1]] = f[2]
    return version, exports, wraps, hdrs


def version_problems(root, new_text):
    """Removing or changing an export needs a major bump, adding one a minor bump, against the merge base with main."""
    ref = cl.merge_base(root)
    if not ref:
        return ["no merge base with main, so the API version bump cannot be checked"]
    r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, TABLE_REL)], capture_output=True, text=True)
    if r.returncode != 0:
        return []  # the change that introduces the table
    old_v, old, _, old_h = parse_table(r.stdout)
    new_v, new, _, new_h = parse_table(new_text)
    return [] if old_v is None or new_v is None else bump_problems(old_v, old, new_v, new, old_h, new_h)


def bump_problems(old_v, old, new_v, new, old_h=None, new_h=None):
    """Exports removed, a prototype hash changed or a header's declarations changed need a major bump; exports added a
    minor bump. A header that joined or left the list is no change by itself: its exports are."""
    broke = sorted(n for n in old if new.get(n) != old[n])
    old_h, new_h = old_h or {}, new_h or {}
    broke += ["types of " + h for h in sorted(old_h) if h in new_h and new_h[h] != old_h[h]]
    added = sorted(n for n in new if n not in old)
    if broke and new_v[0] <= old_v[0]:
        return ["exports removed or changed (%s) need a LICHEN_API_VERSION_MAJOR above %d" % (", ".join(broke[:5]), old_v[0])]
    if added and not broke and new_v <= old_v:
        return ["exports added (%s) need a LICHEN_API_VERSION bump above %d.%d" % (", ".join(added[:5]), old_v[0], old_v[1])]
    return []


def undefined_symbols(path):
    nm = os.environ.get("NM") or shutil.which("riscv32-esp-elf-nm")
    if not nm:
        raise ExportError("riscv32-esp-elf-nm is not on PATH (IDF export.sh), so the apps' symbols cannot be checked")
    try:
        r = subprocess.run([nm, "-u", path], capture_output=True, text=True)
    except OSError as e:
        raise ExportError("cannot run %s: %s" % (nm, e))
    if r.returncode != 0:
        raise ExportError("%s failed on %s: %s" % (nm, path, r.stderr.strip()))
    return sorted(l.split()[-1] for l in r.stdout.splitlines() if l.split())


def elf_problems(paths, exports, apps_src=None):
    """An app whose undefined symbols are not in the table, and (with apps_src) a sdapps/<id>/ with no built app.elf."""
    names = set(e[0] for e in exports)
    out = []
    if apps_src and os.path.isdir(apps_src):
        built = set(os.path.basename(os.path.dirname(p)) for p in paths)
        out += ["sdapps/%s has no built app.elf: nothing checked its symbols" % d
                for d in sorted(os.listdir(apps_src)) if os.path.isdir(os.path.join(apps_src, d)) and d not in built]
    for p in paths:
        missing = [s for s in undefined_symbols(p) if s not in names]
        if missing:
            out.append("%s needs symbols the export table lacks: %s" % (os.path.relpath(p), " ".join(missing)))
    return out


def generate(root):
    conf = cl.parse_conf(cl.read(os.path.join(root, cl.CONF_REL)))
    exports, hdrs = build_exports(root, conf)
    return exports, table_text(exports, find_wraps(root), api_version(root), hdrs)


def check(root, elfs, apps_src=None):
    exports, table = generate(root)
    problems = []
    for rel, want in ((TABLE_REL, table), (C_REL, c_text(exports))):
        path = os.path.join(root, rel)
        if not os.path.exists(path) or cl.read(path) != want:
            problems.append("%s does not match the headers (run: tools/gen_export_table.py --write)" % rel)
    problems += version_problems(root, table)
    problems += elf_problems(elfs, exports, apps_src)
    return problems, len(exports)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--elf", nargs="*", default=[], help="SD apps whose undefined symbols must be in the table")
    ap.add_argument("--apps-src", help="sdapps/ directory: every app in it must have an ELF in --elf")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    try:
        if args.write:
            exports, table = generate(root)
            cl.write_atomic(os.path.join(root, TABLE_REL), table)
            cl.write_atomic(os.path.join(root, C_REL), c_text(exports))
            print("export table: wrote %d exports" % len(exports))
            return 0
        problems, n = check(root, args.elf, args.apps_src)
    except (ExportError, cl.LayerError) as e:
        print("export table: " + str(e), file=sys.stderr)
        return 1
    for p in problems:
        print("export table: " + p, file=sys.stderr)
    if problems:
        return 1
    print("export table: ok (%d exports, %d app(s) checked)" % (n, len(args.elf)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
