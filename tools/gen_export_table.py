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
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_layers as cl  # noqa: E402

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


KEYWORDS = frozenset(("void", "int", "char", "short", "long", "unsigned", "signed", "float", "double", "const",
                      "volatile", "struct", "enum", "union", "bool", "_Bool"))


def normalise_param(p):
    """A parameter without its name, so renaming one is no API change: `char const *fmt` -> `char const *`."""
    p = re.sub(r"\s+", " ", p.strip())
    m = re.match(r"^(.*?[\w\*\)] ?)(\b[A-Za-z_]\w*)\s*(\[[^\]]*\])?$", p)
    if "(" in p or m is None or m.group(2) in KEYWORDS:
        return p
    before = m.group(1).split()
    if not before or before[-1] in ("struct", "enum", "union"):
        return p
    return m.group(1).strip() + (m.group(3) or "")


def split_params(inner):
    out, depth, cur = [], 0, ""
    for ch in inner:
        depth += ch == "("
        depth -= ch == ")"
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    return out + [cur]


def prototype_hash(stmt):
    """Short hash of a declaration's normalised prototype: the return type, the name and the parameter types."""
    stmt = " ".join(cl.ATTR_RE.sub(" ", stmt).split())
    i = stmt.index("(")
    j = stmt.rindex(")")
    ret = stmt[:i].strip()
    params = ",".join(normalise_param(p) for p in split_params(stmt[i + 1:j]))
    return hashlib.sha256(("%s(%s)" % (ret, params)).encode()).hexdigest()[:8]


def header_prototypes(root, conf):
    """{function: (header, prototype hash)} over the headers the surface walk covers."""
    out = {}
    surface = set((f, h) for f, h, _ in cl.surface_functions(root, conf))  # only the headers apps may include
    dirs = [cl.gate_dir(root)]
    comps = cl.repo_components(root)
    for (comp, peer), _globs in sorted(conf.peer_headers.items()):
        if conf.comp_layer.get(comp) in conf.closed and peer in comps:
            dirs.append(comps[peer])
    for d in dirs:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".h"):
                continue
            for stmt, _block in cl.scan_statements(cl.read(os.path.join(d, fn))):
                name = cl.statement_function(stmt)
                if name and (name, fn) in surface:
                    out.setdefault(name, (fn, prototype_hash(stmt)))
    return out


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
    """[(name, symbol, origin, hash)] sorted by name; symbol is what the table points at (a wrapped function or a shim)."""
    wraps = set(find_wraps(root))
    rows = {n: (n, h, ph) for n, (h, ph) in api_selection(root, conf, header_prototypes(root, conf)).items()}
    for name, target in parse_allow(cl.read(os.path.join(root, ALLOW_REL))):
        if name in rows:
            raise ExportError("%s: %s is already exported from %s" % (ALLOW_REL, name, rows[name][1]))
        if target is None and forbidden(conf, name):
            raise ExportError("%s: %s is forbidden to apps (layers.conf); map it to an OS function or drop it" % (ALLOW_REL, name))
        rows[name] = (target or name, "allow", "-")
    out = []
    for name in sorted(rows):
        sym, origin, ph = rows[name]
        if sym in wraps:
            sym = "__wrap_" + sym  # the linker would redirect &sym there anyway; the table names it so the diff shows it
        out.append((name, sym, origin, ph))
    return out


def table_text(exports, wraps, version):
    head = (
        "# Export table of the SD-card apps (#519): what an app's ELF may resolve against the OS. Generated by\n"
        "# `tools/gen_export_table.py --write` from tools/export_api.txt (the curated app API), tools/export_allow.txt and the\n"
        "# forbid lines of tools/layers.conf; checked on every build. Changing it needs a LICHEN_API_VERSION bump: major\n"
        "# when a line is removed or its prototype hash changes, minor when one is added (lichen_api_version.h).\n"
        "# export<TAB>name<TAB>symbol<TAB>origin (header, or allow)<TAB>hash (normalised prototype; - for libc);\n"
        "# wrap<TAB>X<TAB>__wrap_X: every -Wl,--wrap=X (informational: a new wrap needs no bump).\n"
    )
    body = "api\t%d.%d\n" % version
    body += "".join("wrap\t%s\t__wrap_%s\n" % (w, w) for w in wraps)
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
    """(version tuple or None, {name: symbol}, wraps) of an export_table.txt."""
    version, exports, wraps = None, {}, []
    for raw in text.splitlines():
        f = raw.split("\t")
        if f[0] == "api":
            version = tuple(int(x) for x in f[1].split("."))
        elif f[0] == "export":
            exports[f[1]] = f[4] if len(f) > 4 else f[2]  # the prototype hash; a pre-hash table compares by symbol
        elif f[0] == "wrap":
            wraps.append(f[1])
    return version, exports, wraps


def version_problems(root, new_text):
    """Removing or changing an export needs a major bump, adding one a minor bump, against the merge base with main."""
    ref = cl.merge_base(root)
    if not ref:
        return ["no merge base with main, so the API version bump cannot be checked"]
    r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, TABLE_REL)], capture_output=True, text=True)
    if r.returncode != 0:
        return []  # the change that introduces the table
    old_v, old, _ = parse_table(r.stdout)
    new_v, new, _ = parse_table(new_text)
    return [] if old_v is None or new_v is None else bump_problems(old_v, old, new_v, new)


def bump_problems(old_v, old, new_v, new):
    broke = sorted(n for n in old if new.get(n) != old[n])
    added = sorted(n for n in new if n not in old)
    if broke and new_v[0] <= old_v[0]:
        return ["exports removed or changed (%s) need a LICHEN_API_VERSION_MAJOR above %d" % (", ".join(broke[:5]), old_v[0])]
    if added and not broke and new_v <= old_v:
        return ["exports added (%s) need a LICHEN_API_VERSION bump above %d.%d" % (", ".join(added[:5]), old_v[0], old_v[1])]
    return []


def undefined_symbols(path):
    nm = os.environ.get("NM") or "riscv32-esp-elf-nm"
    r = subprocess.run([nm, "-u", path], capture_output=True, text=True)
    if r.returncode != 0:
        raise ExportError("%s failed on %s: %s" % (nm, path, r.stderr.strip()))
    return sorted(l.split()[-1] for l in r.stdout.splitlines() if l.split())


def elf_problems(paths, exports):
    names = set(e[0] for e in exports)
    out = []
    for p in paths:
        missing = [s for s in undefined_symbols(p) if s not in names]
        if missing:
            out.append("%s needs symbols the export table lacks: %s" % (os.path.basename(p), " ".join(missing)))
    return out


def generate(root):
    conf = cl.parse_conf(cl.read(os.path.join(root, cl.CONF_REL)))
    exports = build_exports(root, conf)
    return exports, table_text(exports, find_wraps(root), api_version(root))


def check(root, elfs):
    exports, table = generate(root)
    problems = []
    for rel, want in ((TABLE_REL, table), (C_REL, c_text(exports))):
        path = os.path.join(root, rel)
        if not os.path.exists(path) or cl.read(path) != want:
            problems.append("%s does not match the headers (run: tools/gen_export_table.py --write)" % rel)
    problems += version_problems(root, table)
    problems += elf_problems(elfs, exports)
    return problems, len(exports)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--elf", nargs="*", default=[], help="SD apps whose undefined symbols must be in the table")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    try:
        if args.write:
            exports, table = generate(root)
            cl.write_atomic(os.path.join(root, TABLE_REL), table)
            cl.write_atomic(os.path.join(root, C_REL), c_text(exports))
            print("export table: wrote %d exports" % len(exports))
            return 0
        problems, n = check(root, args.elf)
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
