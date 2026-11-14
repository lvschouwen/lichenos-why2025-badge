#!/usr/bin/env python3
"""Layer checker (#218, design in #216). Run through tools/check_layers.sh (it puts the IDF nm on PATH).

Every repo component has a layer in tools/layers.conf (L0 platform ... L6 apps, main). Three rules:
  A  REQUIRES / PRIV_REQUIRES go to the same or a lower layer (plus listed peers); a closed layer (apps) may only
     name the listed libraries.
  B  #include of a header that belongs to another repo component follows the same edges; a closed layer may only
     include its own headers, allowed components and the listed system headers.
  C  nm on the built archives: an undefined symbol that another component defines follows the same edges (this is
     the link-time upward edge REQUIRES does not show); owner-only symbols (lvgl_port_lock, nvs_open, heap_caps_*,
     xTaskCreate* ...) may only be used by their listed owners; `forbid` symbols may not be used at all. A function
     a lichen_api header declares and lichen_os defines counts as the gate's (#220: the gate is a header API, lichen_os
     implements it), so the caller needs an edge to lichen_api. Only lichen_os gets this; a gate function the shell
     defines stays an upward edge.
A peer edge of a closed layer names the headers it may include (peer-headers, #283); the others of that component are
a hard B. An app's sources are its component directory plus every own-dir root that is not `vendored` (shared code
built into it, e.g. the Arduino shim, #283). `forbid-except` names the one object of a component (upstream code built
unchanged) that may still use a forbidden symbol; it is keyed to the object's file name in the archive
(MyMesh.cpp.obj), so a line whose object no longer exists or no longer uses the symbol is a hard C (a rename upstream
cannot drop the exception silently). An owner-only line with no owner bans the symbol for everyone, hard like forbid.
  D  capabilities (#229): the `cap` list in layers.conf is the CAP_* list of lichen_caps.h, and every app descriptor
     (lichen_app_t ... = {...} in a component's sources) has an `app` line with the same caps and budgets (a secret
     prefix needs SECRETS, a start_background hook BACKGROUND). nm on the closed layer's archives: every undefined
     symbol the manifest gives a capability (`cap-func` / `cap-header`) is used by an object of an app (`app-objects`)
     that declared it. All D failures are hard.

Rule C counts per object file (an object that calls a symbol three times counts once), A per edge, B per include line.
Hard failures, never baselined and never ratcheted: a `forbid` symbol, a library or header outside a closed layer's
list, an unresolved ${VAR} in a REQUIRES list (reported as rule A!, B!, C!). They keep the old app boundary guarantee.

Violations are ratcheted against tools/layers-baseline.txt (metric = rule, key = from -> to or from:symbol, value =
count): a violation that is not in the baseline is NEW, a higher one is WORSE, a baseline entry that is fixed, lowered
or gone is STALE (run --shrink), and a baseline entry that is new or higher than in the merge-base version of the file
GREW. The baseline only shrinks. Report mode (--report) prints every violation and exits 0.

Also: --gen-manifest writes tools/gate_manifest.txt from the lichen_api headers and the peer headers apps may include
(peer-headers of a closed layer, #229: lora_app.h, fw_console.h ...), with the capability layers.conf gives each
function. The manifest must match (a failure otherwise, #220), and rule C takes the gate's functions from the lichen_api
headers themselves, so a header is the only way in: a line added to the manifest by hand frees nothing.

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
GATE = "lichen_api"      # the gate: its headers are the app API
# The services that implement the gate's headers: lichen_os (#220) and lichen_notify (#245, notifications)
GATE_IMPL = frozenset(("lichen_os", "lichen_notify"))
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
        self.owner_only = []        # (glob, [owner spec]) first match wins; no owner: nobody may use it
        self.peer_headers = {}      # (component, peer) -> header globs it may include of that peer
        self.vendored = collections.defaultdict(list)  # component -> own-dir roots of third-party code (not scanned)
        self.forbid_except = []     # (component, object file name glob, [symbol globs]): upstream code built unchanged
        self.caps = {}              # #229: capability name -> bit (lichen_caps.h CAP_<name>)
        self.cap_funcs = []         # (cap spec, [function globs]) first match wins; spec '-', 'A' or 'A|B' (either)
        self.cap_headers = []       # (cap spec, [header globs]) for the functions no cap-func line names
        self.apps = {}              # app id -> App

    def height(self, layer):
        return self.layers.index(layer)

    def allowed_layers(self, layer):
        return self.may.get(layer) or set(self.layers[: self.height(layer) + 1])

    def edge_ok(self, src, dst):
        """May component src depend on repo component dst?"""
        if src == dst or dst in self.peers[src]:
            return True
        return self.comp_layer[dst] in self.allowed_layers(self.comp_layer[src])

    def header_ok(self, src, dst, inc):
        """May src include this header of dst? Only a peer-headers line narrows it (to the headers it names)."""
        globs = self.peer_headers.get((src, dst))
        return globs is None or any(fnmatch.fnmatchcase(os.path.basename(inc), g) for g in globs)

    def forbid_excepted(self, comp, obj, sym):
        return any(c == comp and fnmatch.fnmatchcase(obj or "", o) and any(fnmatch.fnmatchcase(sym, g) for g in syms)
                   for c, o, syms in self.forbid_except)


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
    elif key == "peer-headers":
        if not args[2:]:
            raise ValueError("peer-headers needs a component, a peer and at least one header")
        conf.peer_headers.setdefault((args[0], args[1]), []).extend(args[2:])
    elif key == "vendored":
        conf.vendored[args[0]].extend(args[1:])
    elif key == "forbid-except":
        if not args[2:]:
            raise ValueError("forbid-except needs a component, an object and at least one symbol")
        conf.forbid_except.append((args[0], args[1], args[2:]))
    elif key in CAP_KEYS:
        apply_cap_directive(conf, key, args)
    else:
        raise ValueError("unknown directive")


CAP_KEYS = ("cap", "cap-func", "cap-header", "app", "app-objects")
BUDGETS = ("INTERNAL_RAM", "PSRAM")  # KiB; lichen_app_t internal_ram_kib, psram_kib


class App:
    def __init__(self, app_id, comp, caps, budgets):
        self.id, self.comp, self.caps, self.budgets = app_id, comp, caps, budgets
        self.objects = []  # source file names (globs) whose objects are the app's, for rule D


def apply_cap_directive(conf, key, args):
    """The #229 directives: cap NAME BIT; cap-func SPEC globs; cap-header SPEC globs; app ID COMPONENT CAP... [BUDGET=n];
    app-objects ID globs."""
    if key == "cap":
        conf.caps[args[0]] = int(args[1])
    elif key in ("cap-func", "cap-header"):
        if not args[1:]:
            raise ValueError("%s needs a capability and at least one name" % key)
        (conf.cap_funcs if key == "cap-func" else conf.cap_headers).append((args[0], args[1:]))
    elif key == "app":
        if args[0] in conf.apps:
            raise ValueError("app %s is listed twice" % args[0])
        caps, budgets = set(), {}
        for a in args[2:]:
            if "=" in a:
                name, val = a.split("=", 1)
                if name not in BUDGETS:
                    raise ValueError("unknown budget %s (%s)" % (name, ", ".join(BUDGETS)))
                budgets[name] = int(val)
            elif a != "-":
                caps.add(a)
        conf.apps[args[0]] = App(args[0], args[1], caps, budgets)
    else:
        if args[0] not in conf.apps or not args[1:]:
            raise ValueError("app-objects needs an app of an earlier app line and at least one file")
        conf.apps[args[0]].objects.extend(args[1:])


def check_conf_caps(conf):
    """#229: the capability names on every line are declared `cap` names, bits are unique, apps sit on components."""
    if len(set(conf.caps.values())) != len(conf.caps):
        raise LayerError("layers.conf: two cap lines share a bit")
    names = [n for spec, _ in conf.cap_funcs + conf.cap_headers if spec != "-" for n in spec.split("|")]
    for a in conf.apps.values():
        if a.comp not in conf.comp_layer:
            raise LayerError("layers.conf: app %s names unknown component %s" % (a.id, a.comp))
        names += sorted(a.caps)
    for n in names:
        if n not in conf.caps:
            raise LayerError("layers.conf: capability %s has no cap line" % n)


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
    check_conf_283(conf)
    check_conf_caps(conf)
    for c, layer in conf.comp_layer.items():
        if layer not in conf.layers:
            raise LayerError("layers.conf: component %s is on the undeclared layer %s" % (c, layer))
    for layer in list(conf.may) + list(conf.closed) + list(conf.headers) + list(conf.forbid):
        if layer not in conf.layers:
            raise LayerError("layers.conf: undeclared layer %s" % layer)


def check_conf_283(conf):
    """The #283 directives: peer-headers on a real peer edge (and on every peer edge of a closed layer), vendored roots
    that are own-dir roots, forbid-except on a known component."""
    for (c, d) in conf.peer_headers:
        if c not in conf.comp_layer or d not in conf.peers[c]:
            raise LayerError("layers.conf: peer-headers %s %s names no peer line" % (c, d))
    for c, deps in conf.peers.items():
        if c in conf.comp_layer and conf.comp_layer[c] in conf.closed:
            for d in sorted(deps - set(d for (s, d) in conf.peer_headers if s == c)):
                raise LayerError("layers.conf: peer %s %s is on a closed layer and needs a peer-headers line" % (c, d))
    for c, dirs in conf.vendored.items():
        for d in dirs:
            if d not in conf.own_dirs.get(c, []):
                raise LayerError("layers.conf: vendored %s %s is not an own-dir root of it" % (c, d))
    for c, _, _ in conf.forbid_except:
        if c not in conf.comp_layer:
            raise LayerError("layers.conf: forbid-except names unknown component %s" % c)


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


def source_roots(conf, name, comp_dir, fw):
    """The directories whose sources are the component's: its own directory, and every own-dir root outside it that is
    not vendored (shared code compiled into it, #283). A missing root fails instead of passing unchecked."""
    roots = [comp_dir]
    for o in conf.own_dirs[name]:
        d = os.path.normpath(os.path.join(fw, o))
        inside = os.path.commonpath([d, os.path.normpath(comp_dir)]) == os.path.normpath(comp_dir)
        if inside or o in conf.vendored[name]:
            continue
        if not os.path.isdir(d):
            raise LayerError("own-dir %s %s does not exist (mark third-party code `vendored`)" % (name, o))
        roots.append(d)
    return roots


def rule_b(conf, comps, found, root, selected):
    fw = os.path.join(root, "firmware")
    owners = header_owners(comps)
    for name in selected:
        layer = conf.comp_layer[name]
        for base in source_roots(conf, name, comps[name], fw):
            for path in source_files(base):
                if any(p in "/" + os.path.relpath(path, base).replace(os.sep, "/") for p in SKIP_PARTS):
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
        if name in cands or any(conf.edge_ok(name, c) and conf.header_ok(name, c, inc) for c in cands):
            return None, False
        peers = sorted(c for c in cands if conf.edge_ok(name, c))
        if peers:  # an allowed edge, but not one of the headers its peer-headers line names (#283)
            return "%s:%s" % (peers[0], os.path.basename(inc)), layer in conf.closed
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


def gate_call_ok(conf, comp, sym, owner, gate_syms):
    """A call to a gate-declared function that an implementer (GATE_IMPL: lichen_os, lichen_notify) defines is a call to
    the gate (#220, #245). Not for the gate itself: the gate calling into an implementer is a downward edge it may not
    have (L4 uses L0 and L4 only)."""
    return (owner in GATE_IMPL and comp != GATE and sym in gate_syms and GATE in conf.comp_layer
            and conf.edge_ok(comp, GATE))


def sym_violation(conf, comp, layer, sym, defs, gate_syms=frozenset(), obj=None):
    """(symbol, hard) when this reference breaks a rule, else (None, False). Forbidden symbols are hard unless a
    forbid-except line names this object. gate_syms: the functions the lichen_api headers declare."""
    for g in conf.forbid[layer]:
        if fnmatch.fnmatchcase(sym, g) and not conf.forbid_excepted(comp, obj, sym):
            return sym, True
    for glob, owners in conf.owner_only:
        if fnmatch.fnmatchcase(sym, glob):
            if sym in defs and defs[sym] == comp:
                return None, False
            if not owners:
                return sym, True  # nobody may use it (flash mmap, #283): never baselined
            return (None if any(owner_matches(conf, o, comp) for o in owners) else sym), False
    owner = defs.get(sym)
    if owner and owner != comp and not conf.edge_ok(comp, owner) and not gate_call_ok(conf, comp, sym, owner, gate_syms):
        return sym, False
    return None, False


def unused_forbid_excepts(conf, name, lib, undef, found):
    """A forbid-except line of this component that no object of the archive uses (renamed or fixed upstream) is a hard
    C: the key is checked against the archive, never trusted."""
    for c, o, syms in conf.forbid_except:
        if c == name and not any(fnmatch.fnmatchcase(obj, o) and any(fnmatch.fnmatchcase(s, g) for g in syms)
                                 for obj, s in undef):
            add(found, "C", "%s:forbid-except %s %s (no such use in the archive)" % (name, o, " ".join(syms)), lib, True)


def rule_c(conf, comps, found, build, nm, selected, gate_syms=frozenset()):
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
        undef = undefined_symbols(nm, lib)
        unused_forbid_excepts(conf, name, lib, undef, found)
        for obj, sym in undef:
            if defs.get(sym) == name:
                continue
            bad, is_hard = sym_violation(conf, name, layer, sym, defs, gate_syms, obj)
            if bad:
                objs[bad].add(obj)
                if is_hard:
                    hard.add(bad)
        for sym, o in objs.items():
            for ob in sorted(o):
                add(found, "C", "%s:%s" % (name, sym), "%s(%s)" % (lib, ob), sym in hard)


# --- rule D (capabilities, #229) ------------------------------------------------------------------------------

CAPS_HEADER = "lichen_caps.h"  # the CAP_* list, included by lichen_app.h
CAP_DEFINE_RE = re.compile(r"^\s*#\s*define\s+CAP_(\w+)\s+\(\s*1u\s*<<\s*(\d+)\s*\)", re.M)
# Either qualifier order ("lichen_app_t const x", "const lichen_app_t x"), static or not: a new app cannot escape the
# mirror by spelling its descriptor differently
DESC_RE = re.compile(r"(?:\blichen_app_t\s+const|\bconst\s+lichen_app_t)\s+(\w+)\s*=\s*\{(.*?)\n\s*\};", re.S)
FIELD_RE = re.compile(r"^\s*\.(\w+)\s*=\s*(.+?)\s*,?\s*(?://.*)?$")
STR_DEFINE_RE = re.compile(r'^\s*#\s*define\s+(\w+)\s+"([^"]*)"', re.M)


def cap_satisfied(spec, caps):
    """spec '-' needs nothing, 'A|B' either; SYSTEM (the OS's own apps) has every capability."""
    return spec == "-" or "SYSTEM" in caps or any(c in caps for c in spec.split("|"))


def descriptors(text):
    """[(variable, {field: value})] of the lichen_app_t definitions in a source file (designated initialisers)."""
    out = []
    for m in DESC_RE.finditer(text):
        fields = {}
        for line in m.group(2).splitlines():
            f = FIELD_RE.match(line)
            if f:
                fields[f.group(1)] = f.group(2)
        out.append((m.group(1), fields))
    return out


def caps_of_value(value):
    """The capability names of a .caps initialiser ('0' or CAP_X | CAP_Y), None when it is something else."""
    if value.strip() == "0":
        return set()
    parts = [p.strip() for p in value.split("|")]
    if not all(re.fullmatch(r"CAP_\w+", p) for p in parts):
        return None
    return set(p[4:] for p in parts)


def mirror_defines(conf, root, found):
    """lichen_caps.h's CAP_* bits are layers.conf's cap lines, both ways."""
    path = os.path.join(gate_dir(root), CAPS_HEADER)
    if not conf.caps:
        return
    if not os.path.exists(path):
        add(found, "D", "cap list (no %s to mirror it)" % CAPS_HEADER, CONF_REL, True)
        return
    have = {n: int(b) for n, b in CAP_DEFINE_RE.findall(read(path))}
    for n in sorted(set(have) | set(conf.caps)):
        if have.get(n) != conf.caps.get(n):
            add(found, "D", "cap %s (layers.conf bit %s, lichen_caps.h bit %s)" % (n, conf.caps.get(n), have.get(n)),
                os.path.relpath(path, root), True)


def descriptor_problems(conf, comp, fields, defines):
    """What is wrong with one descriptor against its app line, as strings ([] = fine)."""
    raw_id = fields.get("id", "")
    app_id = raw_id.strip('"') if raw_id.startswith('"') else defines.get(raw_id, raw_id)
    app = conf.apps.get(app_id)
    if not app:
        return app_id, ["no `app %s %s ...` line in %s" % (app_id, comp, CONF_REL)]
    caps = caps_of_value(fields.get("caps", "0"))
    probs = []
    if app.comp != comp:
        probs.append("its app line names component %s, the descriptor is in %s" % (app.comp, comp))
    if caps is None:
        probs.append(".caps must be 0 or CAP_* names joined with |")
    elif caps != app.caps:
        probs.append(".caps %s differ from layers.conf %s" % (" ".join(sorted(caps)) or "-", " ".join(sorted(app.caps)) or "-"))
    for b, member in (("INTERNAL_RAM", "internal_ram_kib"), ("PSRAM", "psram_kib")):
        if fields.get(member, "0") != str(app.budgets.get(b, 0)):
            probs.append(".%s %s differ from layers.conf %s=%s" % (member, fields.get(member, "0"), b, app.budgets.get(b, 0)))
    has = app.caps
    if fields.get("secret_prefix", "NULL") != "NULL" and not cap_satisfied("SECRETS", has):
        probs.append("a secret_prefix needs SECRETS")
    if "SECRETS" in has and fields.get("secret_prefix", "NULL") == "NULL":
        probs.append("SECRETS without a secret_prefix")
    if fields.get("start_background", "NULL") != "NULL" and not cap_satisfied("BACKGROUND", has):
        probs.append("a start_background hook needs BACKGROUND")
    return app_id, probs


def rule_d_mirror(conf, comps, found, root):
    """Every descriptor in a repo component against its app line, and every app line against a descriptor."""
    mirror_defines(conf, root, found)
    seen = set()
    for comp, d in comps.items():
        files = [f for f in source_files(d) if f.endswith((".c", ".cpp"))]
        texts = {f: read(f) for f in files}
        defines = dict(m for t in texts.values() for m in STR_DEFINE_RE.findall(t))
        for f in [h for h in source_files(d) if h.endswith(".h")]:
            defines.update(STR_DEFINE_RE.findall(read(f)))
        for f, text in texts.items():
            for var, fields in descriptors(text):
                app_id, probs = descriptor_problems(conf, comp, fields, defines)
                seen.add(app_id)
                for p in probs:
                    add(found, "D", "%s:%s (%s)" % (comp, app_id, p), "%s (%s)" % (os.path.relpath(f, root), var), True)
    for app_id in sorted(set(conf.apps) - seen):
        add(found, "D", "%s:%s (app line without a descriptor)" % (conf.apps[app_id].comp, app_id), CONF_REL, True)


def object_app(conf, comp, obj):
    """The app whose app-objects line names this object of comp's archive (x.c.obj for x.c), or None."""
    src = obj[:-4] if obj.endswith(".obj") else obj
    for a in conf.apps.values():
        if a.comp == comp and any(fnmatch.fnmatchcase(src, g) for g in a.objects):
            return a
    return None


def rule_d_symbols(conf, found, build, nm, selected, cap_of):
    """Every undefined symbol of a closed layer's archive that has a capability is used by an app that declared it."""
    for name in selected:
        if conf.comp_layer[name] not in conf.closed:
            continue
        lib = os.path.join(build, "esp-idf", name, "lib%s.a" % name)
        for obj, sym in sorted(set(undefined_symbols(nm, lib))):
            need = cap_of.get(sym, "-")
            if need == "-":
                continue
            app = object_app(conf, name, obj)
            if app is None:
                add(found, "D", "%s:%s (needs %s; %s belongs to no app-objects line)" % (name, sym, need, obj),
                    "%s(%s)" % (lib, obj), True)
            elif not cap_satisfied(need, app.caps):
                add(found, "D", "%s:%s (needs %s, not declared)" % (app.id, sym, need), "%s(%s)" % (lib, obj), True)


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

def cap_for(conf, func, header):
    """The capability layers.conf gives a function (#229): its cap-func line, else its header's cap-header line, else
    '-' (none)."""
    for spec, globs in conf.cap_funcs:
        if any(fnmatch.fnmatchcase(func, g) for g in globs):
            return spec
    for spec, globs in conf.cap_headers:
        if any(fnmatch.fnmatchcase(header, g) for g in globs):
            return spec
    return "-"


PROTO_RE = re.compile(r"(?:^|\n)([A-Za-z_][\w \t\*]*?[\s\*](\w+)\s*\([^;{}]*\))\s*;", re.S)
SKIP_PROTO = ("typedef", "static", "extern \"C\"", "return", "define")


def thread_rule(comment):
    c = comment.replace("\n", " ")
    c = re.sub(r"//\s*", " ", c)
    c = re.sub(r"\s+", " ", c)
    if re.search(r"\b(?:[Nn]ot|[Nn]ever) (?:on|from) the LVGL task", c):
        return "not-lvgl"
    if re.search(r"any task", c, re.I):
        return "any"
    if re.search(r"LVGL (?:task|context)", c):
        return "lvgl"
    return "unspecified"


ATTR_RE = re.compile(r"__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)")


def statement_function(stmt):
    """Name of the function a header statement declares, or None (typedefs, static inline, function pointers)."""
    stmt = " ".join(ATTR_RE.sub(" ", stmt).split())
    m = PROTO_RE.search("\n" + stmt + ";")
    if not m or stmt.startswith(SKIP_PROTO) or "(*" in stmt.split("(")[0]:
        return None
    return m.group(2)


def scan_statements(text):
    """[(statement, comment block above it)] at brace depth 0. A { } body is skipped as a whole by brace matching, so
    a static inline function never swallows the declaration after it; several statements on one line are separate and
    share the comment block above that line."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    out, block, cur, depth, body, cont, stale = [], [], "", 0, False, False, False
    for line in text.splitlines():
        s, cur = line.strip(), cur.strip()
        if stale and not cur:
            block, stale = [], False
        if cont or (not cur and s.startswith("#")):
            cont = s.endswith("\\")
            continue
        if not cur and (s.startswith('extern "C"') or not s):
            block = block if s else []
            continue
        if not cur and s.startswith("//"):
            block.append(s)
            continue
        for ch in s.split("//")[0] + " ":
            if ch == "{":
                depth += 1
                body = body or depth == 1
            elif ch == "}":
                depth = max(depth - 1, 0)
                if depth == 0 and body and cur.rstrip().endswith(")"):
                    cur, body, stale = "", False, True  # a function definition ends at its closing brace
            elif depth == 0 and ch == ";":
                out.append((cur, block))
                cur, body, stale = "", False, True
            elif depth == 0:
                cur += ch
    return out


def header_functions(text):
    """[(name, thread rule)] of the C function declarations of a header, in order. Each prototype keeps the comment
    block right above it for the thread rule."""
    out = []
    for stmt, block in scan_statements(text):
        name = statement_function(stmt)
        if name:
            out.append((name, thread_rule("\n".join(block))))
    return out


def gate_dir(root):
    return os.path.join(root, "firmware", "components", GATE)


def header_dir_functions(d, wanted=None):
    """[(function, header, thread rule)] of the headers in directory d (only the basenames in wanted, when given)."""
    out = []
    if not os.path.isdir(d):
        return out
    for fn in sorted(os.listdir(d)):
        if fn.endswith(".h") and (wanted is None or fn in wanted):
            with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
                out.extend((name, fn, thr) for name, thr in header_functions(f.read()))
    return out


def gate_functions(root):
    """[(function, header, thread rule)] over the lichen_api headers (none when the tree has no gate)."""
    return header_dir_functions(gate_dir(root))


def peer_functions(root, conf):
    """#229: the functions of the peer headers a closed layer may include (peer-headers lines), so rule D can map an
    app's lora_send or console_register to a capability. Never gate functions for rule C."""
    out, comps = [], repo_components(root)
    for (comp, peer), globs in sorted(conf.peer_headers.items()):
        if conf.comp_layer.get(comp) in conf.closed and peer in comps:
            names = set(fn for fn in os.listdir(comps[peer]) if any(fnmatch.fnmatchcase(fn, g) for g in globs))
            out.extend(header_dir_functions(comps[peer], names))
    return out


def surface_functions(root, conf):
    """[(function, header, thread rule)] of everything an app may call: the gate and the peer headers"""
    return sorted(set(gate_functions(root) + (peer_functions(root, conf) if conf else [])))


MANIFEST_HEAD = (
    "# Gate manifest (#218, #216, #229): every function the lichen_api headers and the apps' peer headers declare, with\n"
    "# the capability that guards it and the thread it may be called from. Format: function<TAB>capability<TAB>thread<TAB>\n"
    "# header. Generated by `tools/check_layers.sh --gen-manifest`, checked against the headers on every build (#220).\n"
    "# capability: from tools/layers.conf (cap-func, cap-header; '-' = none, A|B = either); rule D checks an app's use.\n"
    "# thread: any, lvgl (LVGL task only), not-lvgl, unspecified (the header comment does not say; fix the comment,\n"
    "# then the entry).\n"
)


def manifest_text(funcs, conf=None):
    rows = sorted(set((f, cap_for(conf, f, h) if conf else "-", t, h) for f, h, t in funcs))
    return MANIFEST_HEAD + "".join("%s\t%s\t%s\t%s\n" % r for r in rows)


def manifest_caps(root, conf):
    """function -> capability spec, for rule D"""
    return {f: cap_for(conf, f, h) for f, h, _ in surface_functions(root, conf)}


def manifest_warnings(root, manifest, conf=None):
    have = set(l.split("\t")[0] for l in manifest.splitlines() if l.strip() and not l.startswith("#"))
    now = set(f for f, _, _ in surface_functions(root, conf))
    out = ["gate manifest: %s is declared in a lichen_api header but not in %s" % (f, MANIFEST_REL) for f in sorted(now - have)]
    out += ["gate manifest: %s is in %s but no header declares it" % (f, MANIFEST_REL) for f in sorted(have - now)]
    return out


def manifest_problems(root, conf=None):
    """Failures when tools/gate_manifest.txt is not what --gen-manifest writes from the headers today (#220)."""
    if not os.path.isdir(gate_dir(root)):
        return []
    mf = os.path.join(root, MANIFEST_REL)
    if not os.path.exists(mf):
        return ["%s is missing (run: tools/check_layers.sh --gen-manifest)" % MANIFEST_REL]
    text = read(mf)
    if text == manifest_text(surface_functions(root, conf), conf):
        return []
    return manifest_warnings(root, text, conf) + [
        "%s does not match the headers and layers.conf (run: tools/check_layers.sh --gen-manifest)" % MANIFEST_REL]


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
    if not layer_filter:  # the mirror is about every component's descriptors (the shell's too)
        rule_d_mirror(conf, comps, found, root)
    if symbols:
        rule_c(conf, comps, found, build, nm, selected, frozenset(f for f, _, _ in gate_functions(root)))
        rule_d_symbols(conf, found, build, nm, selected, manifest_caps(root, conf))
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
    ap.add_argument("--only-layer", help="check only the components of one layer (the app boundary check is L6); "
                    "skips rule D's descriptor mirror by design, which covers every component and runs in the full check")
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
    print("layers: %d violation(s) in %d entries (A %d, B %d, C %d, D %d)" % (total, len(found), per["A"], per["B"], per["C"],
                                                                         per["D"]))


def run(args):
    root = os.path.abspath(args.root)
    conf = parse_conf(read(os.path.join(root, CONF_REL)))
    if args.gen_manifest:
        write_atomic(os.path.join(root, MANIFEST_REL), manifest_text(surface_functions(root, conf), conf))
        return 0
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
    return finish(root, fails, measured, baseline, conf)


def finish(root, fails, measured, baseline, conf=None):
    fails = fails + manifest_problems(root, conf)
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
