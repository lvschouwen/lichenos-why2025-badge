#!/usr/bin/env python3
"""Layer checker (#218, design in #216). Run through tools/check_layers.sh (it puts the IDF nm on PATH).

Every repo component has a layer in tools/layers.conf (L0 platform ... L6 apps, main). Three rules:
  A  REQUIRES / PRIV_REQUIRES go to the same or a lower layer (plus listed peers); a closed layer (apps) may only
     name the listed libraries.
  B  #include of a header that belongs to another repo component follows the same edges; a closed layer may only
     include its own headers, allowed components and the listed system headers.
  C  nm on the built archives: an undefined symbol that another component defines follows the same edges (this is
     the link-time upward edge REQUIRES does not show); owner-only symbols (lvgl_port_lock, nvs_open, heap_caps_*,
     xTaskCreate* ...) may only be used by their listed owners; `forbid` symbols may not be used at all.
Rule D (capabilities) is #229's and not here.

Rule C counts per object file (an object that calls a symbol three times counts once), A per edge, B per include line.
Hard failures, never baselined and never ratcheted: a `forbid` symbol, a library or header outside a closed layer's
list, an unresolved ${VAR} in a REQUIRES list (reported as rule A!, B!, C!). They keep the old app boundary guarantee.

Violations are ratcheted against tools/layers-baseline.txt (metric = rule, key = from -> to or from:symbol, value =
count): a violation that is not in the baseline is NEW, a higher one is WORSE, a baseline entry that is fixed, lowered
or gone is STALE (run --shrink), and a baseline entry that is new or higher than in the merge-base version of the file
GREW. The baseline only shrinks. Report mode (--report) prints every violation and exits 0.

Also: --gen-manifest writes tools/gate_manifest.txt from the lichen_api headers; a header function missing there is a
warning (the capability column is #229's).

Nothing passes silently: a tool that is missing or a build that lacks an archive fails rule C instead of skipping it,
unless --no-symbols asked for the skip."""
import argparse
import collections
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile

RULES = ("A", "B", "C")
CONF_REL = "tools/layers.conf"
BASELINE_REL = "tools/layers-baseline.txt"
MANIFEST_REL = "tools/gate_manifest.txt"
SKIP_PARTS = ("/build/", "/managed_components/")
SOURCE_EXT = (".c", ".cpp", ".h", ".hpp")
REQ_KEYS = ("REQUIRES", "PRIV_REQUIRES")


class LayerError(Exception):
    pass


# --- config ---------------------------------------------------------------------------------------------------

class Conf:
    def __init__(self):
        self.layers = []            # names in order, index = height
        self.comp_layer = {}        # component -> layer name
        self.may = {}               # layer -> set of layers it may depend on (default: own and lower)
        self.peers = collections.defaultdict(set)  # component -> extra allowed component dependencies
        self.closed = {}            # layer -> set of allowed external libraries
        self.headers = collections.defaultdict(list)  # layer -> system header globs
        self.comp_headers = collections.defaultdict(list)  # component -> extra system header globs
        self.own_dirs = collections.defaultdict(list)  # component -> include roots relative to firmware/
        self.forbid = collections.defaultdict(list)    # layer -> symbol globs
        self.owner_only = []        # (glob, [owner spec]) first match wins

    def height(self, layer):
        return self.layers.index(layer)

    def allowed_layers(self, layer):
        return self.may.get(layer) or set(self.layers[: self.height(layer) + 1])

    def edge_ok(self, src, dst):
        """May component src depend on repo component dst?"""
        if src == dst or dst in self.peers[src]:
            return True
        return self.comp_layer[dst] in self.allowed_layers(self.comp_layer[src])


def parse_conf(text):
    conf = Conf()
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].split()
        if not line:
            continue
        key, args = line[0], line[1:]
        try:
            apply_directive(conf, key, args)
        except (IndexError, ValueError) as e:
            raise LayerError("layers.conf line %d: bad '%s' directive (%s)" % (n, key, e))
    check_conf(conf)
    return conf


def apply_directive(conf, key, args):
    if key == "layer":
        conf.layers.append(args[0])
    elif key == "component":
        for c in args[:-1]:
            if c in conf.comp_layer:
                raise ValueError("component %s is listed twice" % c)
            conf.comp_layer[c] = args[-1]
    elif key == "layer-may":
        conf.may[args[0]] = set(args[1:])
    elif key == "peer":
        conf.peers[args[0]].update(args[1:])
    elif key == "closed":
        conf.closed.setdefault(args[0], set()).update(args[1:])
    elif key == "headers":
        conf.headers[args[0]].extend(args[1:])
    elif key == "comp-headers":
        conf.comp_headers[args[0]].extend(args[1:])
    elif key == "own-dir":
        conf.own_dirs[args[0]].extend(args[1:])
    elif key == "forbid":
        conf.forbid[args[0]].extend(args[1:])
    elif key == "owner-only":
        conf.owner_only.append((args[0], args[1:]))
    else:
        raise ValueError("unknown directive")


def check_conf(conf):
    for c, deps in conf.peers.items():
        for d in [c] + sorted(deps):
            if d not in conf.comp_layer:
                raise LayerError("layers.conf: peer line names unknown component %s" % d)
    for c in conf.own_dirs:
        if c not in conf.comp_layer:
            raise LayerError("layers.conf: own-dir line names unknown component %s" % c)
    for c in conf.comp_headers:
        if c not in conf.comp_layer:
            raise LayerError("layers.conf: comp-headers line names unknown component %s" % c)
    for glob, owners in conf.owner_only:
        for o in owners:
            if not o.startswith("<=") and o not in conf.comp_layer:
                raise LayerError("layers.conf: owner-only %s names unknown owner %s" % (glob, o))
    for c, layer in conf.comp_layer.items():
        if layer not in conf.layers:
            raise LayerError("layers.conf: component %s is on the undeclared layer %s" % (c, layer))
    for layer in list(conf.may) + list(conf.closed) + list(conf.headers) + list(conf.forbid):
        if layer not in conf.layers:
            raise LayerError("layers.conf: undeclared layer %s" % layer)


def owner_matches(conf, spec, comp):
    """spec is a component name, or '<=Ln' for every component on layer Ln or lower."""
    if spec.startswith("<="):
        layer = spec[2:]
        return layer in conf.layers and conf.height(conf.comp_layer[comp]) <= conf.height(layer)
    return spec == comp


# --- tree facts -----------------------------------------------------------------------------------------------

def repo_components(root):
    """name -> directory of every component of this repo (firmware/components/* and firmware/main)."""
    out = {}
    base = os.path.join(root, "firmware", "components")
    if os.path.isdir(base):
        for name in sorted(os.listdir(base)):
            if os.path.isfile(os.path.join(base, name, "CMakeLists.txt")):
                out[name] = os.path.join(base, name)
    main = os.path.join(root, "firmware", "main")
    if os.path.isfile(os.path.join(main, "CMakeLists.txt")):
        out["main"] = main
    return out


def parse_requires(text):
    """Component names after REQUIRES / PRIV_REQUIRES, ${VAR} expanded from set(VAR ...), with their line."""
    toks = []
    for n, line in enumerate(text.splitlines(), 1):
        line = re.sub(r"#.*", "", line).replace("(", " ( ").replace(")", " ) ")
        toks.extend((n, w.strip('"')) for w in line.split())
    setv, out, state, var = {}, [], "", ""
    for n, w in toks:
        if state == "set-open":
            state = "set-name" if w == "(" else ""
        elif state == "set-name":
            var, state = w, "set"
            setv[var] = []
        elif state == "set":
            if w == ")":
                state = ""
            else:
                setv[var].append((n, w))
        elif w == "set":
            state = "set-open"
        elif w in REQ_KEYS:
            state = "req"
        elif state == "req" and (w == ")" or re.fullmatch(r"[A-Z_]+", w)):
            state = ""
        elif state == "req":
            out.append((n, w))
    return expand_vars(out, setv)


def expand_vars(items, setv):
    res = []
    for n, w in items:
        m = re.fullmatch(r"\$\{(\w+)\}", w)
        if m and m.group(1) in setv:
            res.extend(setv.get(m.group(1), []))
        else:
            res.append((n, w))  # a leftover ${...} is reported by rule A, never dropped
    return res


def source_files(directory):
    for dp, dns, fns in os.walk(directory):
        dns[:] = [d for d in dns if d != "build"]
        for fn in fns:
            if fn.endswith(SOURCE_EXT):
                yield os.path.join(dp, fn)


def header_owners(comps):
    """header basename -> set of repo components that carry a header of that name."""
    owners = collections.defaultdict(set)
    for name, d in comps.items():
        for f in source_files(d):
            if f.endswith((".h", ".hpp")):
                owners[os.path.basename(f)].add(name)
    return owners


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')


def includes_of(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            m = INCLUDE_RE.match(line)
            if m:
                yield n, m.group(1)


# --- rules A and B --------------------------------------------------------------------------------------------

def add(found, rule, key, where, hard=False):
    found[(rule + "!" if hard else rule, key)].append(where)


def rule_a(conf, comps, found, root, selected):
    for name in selected:
        d = comps[name]
        cm = os.path.join(d, "CMakeLists.txt")
        with open(cm, encoding="utf-8") as f:
            reqs = parse_requires(f.read())
        layer = conf.comp_layer[name]
        seen = set()
        for n, dep in reqs:
            if dep in seen or dep == name:
                continue
            seen.add(dep)
            where = "%s:%d" % (os.path.relpath(cm, root), n)
            if "$" in dep:
                add(found, "A", "%s -> %s (unresolved variable)" % (name, dep), where, hard=True)
            elif dep in comps:
                if not conf.edge_ok(name, dep):
                    add(found, "A", "%s -> %s" % (name, dep), where)
            elif layer in conf.closed and dep not in conf.closed[layer]:
                add(found, "A", "%s -> %s" % (name, dep), where, hard=True)


def own_include(conf, name, fw, path, inc):
    for o in conf.own_dirs[name]:
        if os.path.exists(os.path.join(fw, o, inc)):
            return True
    return os.path.exists(os.path.join(os.path.dirname(path), inc))


def rule_b(conf, comps, found, root, selected):
    fw = os.path.join(root, "firmware")
    owners = header_owners(comps)
    for name in selected:
        layer = conf.comp_layer[name]
        for path in source_files(comps[name]):
            if any(p in "/" + os.path.relpath(path, comps[name]).replace(os.sep, "/") for p in SKIP_PARTS):
                continue
            for n, inc in includes_of(path):
                bad, hard = include_violation(conf, name, layer, fw, path, inc, owners)
                if bad:
                    add(found, "B", "%s -> %s" % (name, bad), "%s:%d" % (os.path.relpath(path, root), n), hard)


def include_violation(conf, name, layer, fw, path, inc, owners):
    """(key target, hard) of a violating include: a component, or the header itself for a closed layer; (None, False)
    when it is fine. A header outside a closed layer's list is a hard failure, an upward component include is not."""
    if own_include(conf, name, fw, path, inc):
        return None, False
    cands = owners.get(os.path.basename(inc), set())
    if cands:
        if name in cands or any(conf.edge_ok(name, c) for c in cands):
            return None, False
        return sorted(cands)[0], False
    if layer in conf.closed and not any(
            fnmatch.fnmatchcase(inc, g) for g in conf.headers[layer] + conf.comp_headers[name]):
        return inc, True
    return None, False


# --- rule C ---------------------------------------------------------------------------------------------------

def run_nm(nm, args, lib):
    r = subprocess.run([nm] + args + [lib], capture_output=True, text=True)
    if r.returncode != 0:
        raise LayerError("%s %s failed: %s" % (nm, lib, r.stderr.strip()))
    return r.stdout


UNDEF_RE = re.compile(r"^(?:.*?):(?P<obj>[^:]+):\s+U (?P<sym>\S+)$")
DEF_RE = re.compile(r"^[0-9a-fA-F]*\s+[TDBRWVSGCI] (?P<sym>\S+)$")


def defined_symbols(nm, libs):
    """symbol -> defining component (first strong definition wins; a weak one only if nothing else defines it)."""
    strong, weak = {}, {}
    for comp, lib in libs.items():
        for line in run_nm(nm, ["-g", "--defined-only"], lib).splitlines():
            m = re.match(r"^[0-9a-fA-F]*\s+(\S) (\S+)$", line)
            if not m:
                continue
            (weak if m.group(1) in "WVwv" else strong).setdefault(m.group(2), comp)
    for sym, comp in weak.items():
        strong.setdefault(sym, comp)
    return strong


def undefined_symbols(nm, lib):
    """[(object, symbol)] of every undefined (strong) reference in an archive."""
    out = []
    for line in run_nm(nm, ["-A", "-u"], lib).splitlines():
        m = UNDEF_RE.match(line)
        if m:
            out.append((m.group("obj"), m.group("sym")))
    return out


def sym_violation(conf, comp, layer, sym, defs):
    """(symbol, hard) when this reference breaks a rule, else (None, False). Forbidden symbols are hard."""
    for g in conf.forbid[layer]:
        if fnmatch.fnmatchcase(sym, g):
            return sym, True
    for glob, owners in conf.owner_only:
        if fnmatch.fnmatchcase(sym, glob):
            if sym in defs and defs[sym] == comp:
                return None, False
            return (None if any(owner_matches(conf, o, comp) for o in owners) else sym), False
    owner = defs.get(sym)
    if owner and owner != comp and not conf.edge_ok(comp, owner):
        return sym, False
    return None, False


def rule_c(conf, comps, found, build, nm, selected):
    libs = {}
    for name in comps:
        lib = os.path.join(build, "esp-idf", name, "lib%s.a" % name)
        if not os.path.isfile(lib):
            raise LayerError("no %s: the symbol rule (C) needs the built archives (run build.sh build; --no-symbols skips C)" % lib)
        libs[name] = lib
    defs = defined_symbols(nm, libs)
    for name in selected:
        lib = libs[name]
        layer = conf.comp_layer[name]
        objs = collections.defaultdict(set)
        hard = set()
        for obj, sym in undefined_symbols(nm, lib):
            if defs.get(sym) == name:
                continue
            bad, is_hard = sym_violation(conf, name, layer, sym, defs)
            if bad:
                objs[bad].add(obj)
                if is_hard:
                    hard.add(bad)
        for sym, o in objs.items():
            for ob in sorted(o):
                add(found, "C", "%s:%s" % (name, sym), "%s(%s)" % (lib, ob), sym in hard)


# --- baseline -------------------------------------------------------------------------------------------------

def parse_baseline(text):
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        p = line.split("\t")
        if len(p) == 4 and p[3].startswith("#"):
            p = p[:3]  # the optional mark (baseline_marks) is not part of the count
        if len(p) != 3 or p[0] not in RULES or not p[2].strip().isdigit():
            raise LayerError("layers-baseline line %d is not 'rule<TAB>key<TAB>count' (rule A, B or C): %r" % (n, line))
        if (p[0], p[1]) in out:
            raise LayerError("layers-baseline line %d: %s %s is listed twice" % (n, p[0], p[1]))
        out[(p[0], p[1])] = int(p[2])
    return out


def baseline_marks(text):
    """{(rule, key): mark} of the entries that carry a trailing '# ...' field (moved debt, see check_grew)."""
    out = {}
    for line in text.splitlines():
        p = line.split("\t")
        if len(p) == 4 and p[3].startswith("#"):
            out[(p[0], p[1])] = p[3]
    return out


def format_baseline(header, entries, marks=None):
    lines = [header.rstrip("\n")]
    lines += ["%s\t%s\t%d%s" % (r, k, v, "\t" + marks[(r, k)] if marks and (r, k) in marks else "")
              for (r, k), v in sorted(entries.items())]
    return "\n".join(lines) + "\n"


def split_header(text):
    head = []
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            head.append(line)
        else:
            break
    return "\n".join(head)


def compare(measured, baseline, rules=RULES, where=None):
    """The ratchet. measured/baseline: {(rule, key): count}. Returns (failures, stale) as lists of strings."""
    fails, stale = [], []
    for (rule, key), n in sorted(measured.items()):
        if rule.endswith("!"):
            at = " at " + "; ".join((where or {}).get((rule, key), [])[:3]) if where else ""
            fails.append("HARD  %s %s (x%d, never baselined)%s" % (rule[0], key, n, at))
            continue
        if rule not in rules:
            continue
        base = baseline.get((rule, key))
        at = " at " + "; ".join((where or {}).get((rule, key), [])[:3]) if where else ""
        if base is None:
            fails.append("NEW   %s %s (x%d)%s" % (rule, key, n, at))
        elif n > base:
            fails.append("WORSE %s %s = %d (baseline %d)%s" % (rule, key, n, base, at))
    for (rule, key), base in sorted(baseline.items()):
        if rule not in rules:
            continue
        cur = measured.get((rule, key))
        if cur is None:
            stale.append("%s %s: gone (was %d)" % (rule, key, base))
        elif cur < base:
            stale.append("%s %s: %d -> %d" % (rule, key, base, cur))
    return fails, stale


ALLOW_GROW_ENV = "BADGE_LAYERS_ALLOW_GROW"


def allow_ok(allow, mark):
    """True when allow is a plain issue number and the mark names exactly that issue ('#221' is not '#22')."""
    return bool(allow) and allow.isdigit() and re.search(r"(?:^|\s)#%s(?!\d)" % allow, mark) is not None


def check_grew(baseline, base_baseline, marks=None, allow=None):
    """The 'baseline only shrinks' rule against the merge-base version of the file. allow (an issue number, from
    BADGE_LAYERS_ALLOW_GROW) is a one-off escape for NEW entries only: an entry whose mark names '#<allow>' passes (moved
    debt, a move that cannot be done without the gate function that issue adds). Raised counts are never allowed."""
    out = []
    for (rule, key), v in sorted(baseline.items()):
        b = base_baseline.get((rule, key))
        if b is None and rule == "C" and allow_ok(allow, (marks or {}).get((rule, key), "")):
            print("layers: WARNING: %s=%s lets the baseline grow by %s %s (marked %r); never set it in build.sh or a hook, "
                  "the entry goes away with #%s" % (ALLOW_GROW_ENV, allow, rule, key, marks[(rule, key)], allow),
                  file=sys.stderr)
        elif b is None:
            out.append("GREW  baseline %s %s = %d is not in the merge-base baseline (new entries are not allowed)" % (rule, key, v))
        elif v > b:
            out.append("GREW  baseline %s %s raised %d -> %d (a baseline only shrinks)" % (rule, key, b, v))
    return out


def shrink(measured, baseline, rules=RULES):
    """Baseline entries dropped or lowered to the measured value; never added or raised."""
    out = {}
    for (rule, key), base in baseline.items():
        if rule not in rules:
            out[(rule, key)] = base
            continue
        cur = measured.get((rule, key))
        if cur is not None:
            out[(rule, key)] = min(base, cur)
    return out


def _git_ok(root, *args):
    r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def merge_base(root):
    """The merge base with main. Local main and origin/main are both tried and the newer base wins, because
    origin/main is stale in the worktrees. "" when there is none (shallow clone, no main, no git)."""
    bases = [b for b in (_git_ok(root, "merge-base", "HEAD", ref) for ref in ("main", "origin/main")) if b]
    best = ""
    for b in bases:
        if not best or subprocess.run(["git", "-C", root, "merge-base", "--is-ancestor", best, b]).returncode == 0:
            best = b
    return best


def base_baseline_text(root, ref):
    """The baseline at the merge base; None when it is not there yet (the change that introduces it)."""
    r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, BASELINE_REL)], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def grew_problems(root, baseline, skip, marks=None):
    """GREW failures against the merge-base baseline. Without a merge base the check cannot run: that is a failure
    (not a pass) unless --no-merge-base asked for the skip; the commit that introduces the baseline has nothing to
    compare with and passes."""
    if skip:
        print("layers: warning: GREW check skipped (--no-merge-base)", file=sys.stderr)
        return []
    ref = merge_base(root)
    if not ref:
        print("layers: warning: no merge base with main or origin/main (shallow clone? no git?)", file=sys.stderr)
        return ["no merge base, so the baseline cannot be compared with main (--no-merge-base skips on purpose)"]
    text = base_baseline_text(root, ref)
    return [] if text is None else check_grew(baseline, parse_baseline(text), marks, os.environ.get(ALLOW_GROW_ENV))


# --- gate manifest --------------------------------------------------------------------------------------------

# Seed capability per header for the first generation; #229 owns the real capability list.
HEADER_CAP = {
    "lichen_fs.h": "SD", "lichen_haptics.h": "HAPTICS", "lichen_sensors.h": "SENSORS", "lichen_task.h": "BACKGROUND",
    "lichen_legacy.h": "RAW_KEYS", "lichen_radio.h": "RADIO", "settings_backup.h": "SYSTEM",
    "settings_backup_io.h": "SYSTEM", "lichen_crashlog.h": "SYSTEM", "lichen_badge.h": "SYSTEM",
}
PROTO_RE = re.compile(r"(?:^|\n)([A-Za-z_][\w \t\*]*?[\s\*](\w+)\s*\([^;{}]*\))\s*;", re.S)
SKIP_PROTO = ("typedef", "static", "extern \"C\"", "return", "define")


def thread_rule(comment):
    c = comment.replace("\n", " ")
    c = re.sub(r"//\s*", " ", c)
    c = re.sub(r"\s+", " ", c)
    if re.search(r"\b[Nn]ot (?:on|from) the LVGL task", c):
        return "not-lvgl"
    if re.search(r"any task", c):
        return "any"
    if re.search(r"LVGL (?:task|context)", c):
        return "lvgl"
    return "unspecified"


def header_functions(text):
    """[(name, thread rule)] of the C function declarations of a header, in order. Each prototype keeps the comment
    block right above it for the thread rule."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    out, block, buf, depth = [], [], [], 0
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#"):
            continue
        if s.startswith('extern "C"'):
            continue  # the C++ guard is not a block of declarations
        if s.startswith("//"):
            if not buf:
                block.append(s)
            continue
        if not s:
            if not buf:
                block = []
            continue
        buf.append(s.split("//")[0])
        depth += s.count("{") - s.count("}")
        joined = " ".join(buf)
        if ";" in joined or "{" in joined:
            m = PROTO_RE.search("\n" + joined)
            if m and depth == 0 and not joined.lstrip().startswith(SKIP_PROTO) and "(*" not in joined.split("(")[0]:
                out.append((m.group(2), thread_rule("\n".join(block))))
            buf, block = [], []
    return out


def gate_functions(root):
    """[(function, header, thread rule)] over the lichen_api headers."""
    d = os.path.join(root, "firmware", "components", "lichen_api")
    out = []
    for fn in sorted(os.listdir(d)):
        if fn.endswith(".h"):
            with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
                out.extend((name, fn, thr) for name, thr in header_functions(f.read()))
    return out


MANIFEST_HEAD = (
    "# Gate manifest (#218, #216): every function the lichen_api headers declare, with the capability that guards it and\n"
    "# the thread it may be called from. Format: function<TAB>capability<TAB>thread<TAB>header.\n"
    "# Generated once by `tools/check_layers.sh --gen-manifest`; the capability column is a seed (per header, '-' = none)\n"
    "# and #229 (capabilities) owns making it right. thread: any, lvgl (LVGL task only), not-lvgl, unspecified (the header\n"
    "# comment does not say; fix the comment, then the entry).\n"
)


def manifest_text(funcs):
    rows = sorted(set((f, HEADER_CAP.get(h, "-"), t, h) for f, h, t in funcs))
    return MANIFEST_HEAD + "".join("%s\t%s\t%s\t%s\n" % r for r in rows)


def manifest_warnings(root, manifest):
    have = set(l.split("\t")[0] for l in manifest.splitlines() if l.strip() and not l.startswith("#"))
    now = set(f for f, _, _ in gate_functions(root))
    out = ["gate manifest: %s is declared in a lichen_api header but not in %s" % (f, MANIFEST_REL) for f in sorted(now - have)]
    out += ["gate manifest: %s is in %s but no header declares it" % (f, MANIFEST_REL) for f in sorted(have - now)]
    return out


# --- driver ---------------------------------------------------------------------------------------------------

def measure(conf, root, build, nm, symbols, layer_filter=None):
    comps = repo_components(root)
    unlisted = sorted(set(comps) - set(conf.comp_layer))
    if unlisted:
        raise LayerError("component(s) without a layer in %s: %s" % (CONF_REL, ", ".join(unlisted)))
    selected = [c for c in comps if not layer_filter or conf.comp_layer[c] == layer_filter]
    found = collections.defaultdict(list)
    rule_a(conf, comps, found, root, selected)
    rule_b(conf, comps, found, root, selected)
    if symbols:
        rule_c(conf, comps, found, build, nm, selected)
    return found


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write_atomic(path, text):
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--build", help="firmware build dir (default <root>/firmware/build)")
    ap.add_argument("--nm", help="nm to use (default riscv32-esp-elf-nm from PATH)")
    ap.add_argument("--no-symbols", action="store_true", help="skip rule C (no built archives yet)")
    ap.add_argument("--only-layer", help="check only the components of one layer (the app boundary check is L6)")
    ap.add_argument("--report", action="store_true", help="print every violation with its place, exit 0")
    ap.add_argument("--shrink", action="store_true", help="drop or lower baseline entries that were fixed (never adds)")
    ap.add_argument("--init", action="store_true", help="write the first baseline from what is measured now")
    ap.add_argument("--no-merge-base", action="store_true", help="skip the GREW check against main (tests, no git)")
    ap.add_argument("--gen-manifest", action="store_true", help="write tools/gate_manifest.txt from the lichen_api headers")
    return ap.parse_args(argv)


def report(found):
    total = 0
    for (rule, key), wheres in sorted(found.items()):
        total += len(wheres)
        print("%s %s (x%d)" % (rule, key, len(wheres)))
        for w in wheres:
            print("    " + w)
    per = collections.Counter(r[0] for (r, _k), w in found.items() for _ in w)
    print("layers: %d violation(s) in %d entries (A %d, B %d, C %d)" % (total, len(found), per["A"], per["B"], per["C"]))


def run(args):
    root = os.path.abspath(args.root)
    if args.gen_manifest:
        write_atomic(os.path.join(root, MANIFEST_REL), manifest_text(gate_functions(root)))
        return 0
    conf = parse_conf(read(os.path.join(root, CONF_REL)))
    build = args.build or os.path.join(root, "firmware", "build")
    nm = args.nm or shutil.which("riscv32-esp-elf-nm")
    symbols = not args.no_symbols
    if symbols and not nm:
        raise LayerError("riscv32-esp-elf-nm is not on PATH (IDF export.sh), so the symbol rule (C) cannot run; --no-symbols skips it")
    found = measure(conf, root, build, nm, symbols, args.only_layer)
    if args.report:
        report(found)
        return 0
    return ratchet(args, root, conf, {k: len(v) for k, v in found.items()}, symbols, found)


def ratchet(args, root, conf, all_measured, symbols, where=None):
    path = os.path.join(root, BASELINE_REL)
    rules = RULES if symbols else ("A", "B")
    measured = {k: v for k, v in all_measured.items() if not k[0].endswith("!")}
    if (args.init or args.shrink) and args.only_layer:
        raise LayerError("--init and --shrink work on the whole baseline, not with --only-layer")
    if args.init:
        if len(measured) != len(all_measured):
            raise LayerError("hard violations (never baselined) exist; fix them first (run with --report)")
        if os.path.exists(path):
            raise LayerError("%s exists; --init refuses to overwrite it" % BASELINE_REL)
        write_atomic(path, format_baseline("# Layer baseline (#218), see tools/layers.conf. rule<TAB>key<TAB>count.", measured))
        print("layers: wrote %d baseline entries" % len(measured))
        return 0
    text = read(path)
    baseline = parse_baseline(text)
    if args.only_layer:
        # the baseline of other layers is not in play: keep only entries whose first component is on this layer
        baseline = {k: v for k, v in baseline.items() if conf.comp_layer.get(re.split(r"[ :]", k[1])[0]) == args.only_layer}
    if args.shrink:
        new = shrink(measured, parse_baseline(text), rules)
        write_atomic(path, format_baseline(split_header(text), new, baseline_marks(text)))
        print("layers: baseline now %d entries (was %d)" % (len(new), len(parse_baseline(text))))
        return 0
    fails, stale = compare(all_measured, baseline, rules, where)
    fails += ["STALE baseline %s (run: tools/check_layers.sh --shrink)" % s for s in stale]
    fails += grew_problems(root, baseline, args.no_merge_base, baseline_marks(text))
    return finish(root, fails, measured, baseline)


def finish(root, fails, measured, baseline):
    mf = os.path.join(root, MANIFEST_REL)
    if os.path.exists(mf):
        for w in manifest_warnings(root, read(mf)):
            print("warn: " + w, file=sys.stderr)
    for f in fails:
        print("layers: " + f, file=sys.stderr)
    if fails:
        print("layers: %d problem(s). Fix the code, never raise the baseline (tools/layers.conf, docs/architecture.md)." % len(fails), file=sys.stderr)
        return 1
    print("layers: ok (%d baselined entries, %d measured)" % (len(baseline), len(measured)))
    return 0


def main(argv):
    try:
        return run(parse_args(argv))
    except LayerError as e:
        print("layers: " + str(e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
