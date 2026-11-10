#!/usr/bin/env python3
"""Code and resource standard gate (#202). Limits and rationale: docs/code-standard.md.

Run by firmware/build.sh build (through tools/check_standard.sh) after the P4 image is built. Run it with
`python3 -I tools/check_standard.py [--report | --shrink | --size-update | --rename OLD NEW | --init]`.

Static metrics (lizard: lines per file, lines/CCN/nesting/parameters per function; -fstack-usage: stack frame per
function) are ratcheted: a value past the fail line fails unless tools/standard-baseline.txt lists it at that value or
more. The baseline may only shrink, and the gate enforces that against the merge-base version of the file.
Warn-line hits print for the files changed since the merge base.
Size: the app image against its OTA slot, the C6 image against its slot, and every linked P4 archive against the
sizes at the merge base with origin/main (growth per branch or push, #297; an unreadable reference fails, `--no-ref`
skips it on purpose). A green run rewrites the size lines to the measured values (the commit carries them); a red run
never writes. `--size-update` records growth past the fail line.
Merged string/constant pools (SHF_MERGE, `.str1.4`, `.cst8`) are the image-wide pool of the whole link, not one
archive's: they are taken out of the archive numbers and gated as the line `<merged-strings>`.

Nothing passes silently: a tool that is missing, fails, or measures less than was asked for fails the gate."""
import csv
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# metric: (warn, fail). A value above warn warns, above fail fails.
LIMITS = {
    "nloc": (800, 1500),
    "len": (60, 150),
    "ccn": (15, 25),
    "nest": (4, 6),
    "params": (6, 8),
    "stack": (512, 1024),
}
MOVED = "moved"  # 'moved<TAB>metric:old=>new<TAB>0': a deliberate rename, see apply_rename
BASELINE_METRICS = set(LIMITS) | {"size", MOVED}
IMAGE_PCT = (75.0, 90.0)  # image / OTA slot (measured 62.6 % P4, 48.1 % C6 when set, #297)
GROWTH = (2048, 8192)  # bytes one archive may grow in one change, against the sizes the parent commit recorded
OUR_DIRS = ("firmware/components/", "firmware/main/", "c6/main/", "shared/")
SKIP_PARTS = ("/thirdparty/", "/managed_components/", "/build/")
SOURCE_EXT = (".c", ".cpp", ".h", ".hpp")
BASELINE_REL = "tools/standard-baseline.txt"
DEFAULT_HEADER = "# Code and resource standard baseline (#202), see docs/code-standard.md. metric<TAB>key<TAB>value."


class GateError(Exception):
    pass


# --- baseline ---------------------------------------------------------------------------------------------------

def parse_baseline(text):
    """Lines 'metric<TAB>key<TAB>value'; '#' lines and blanks are comments. Returns {(metric, key): int}."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3 or not parts[2].strip().isdigit():
            raise GateError("baseline line %d is not 'metric<TAB>key<TAB>value': %r" % (n, line))
        if parts[0] not in BASELINE_METRICS:
            raise GateError("baseline line %d: unknown metric %r (known: %s)" % (n, parts[0], ", ".join(sorted(BASELINE_METRICS))))
        entry = (parts[0], parts[1])
        if entry in out:
            raise GateError("baseline line %d: %s %s is listed twice" % (n, parts[0], parts[1]))
        out[entry] = int(parts[2])
    return out


def format_baseline(header, entries):  # sorted: two slots recording sizes produce the same text for the same sizes
    lines = [header.rstrip("\n")]
    for (metric, key), value in sorted(entries.items()):
        lines.append("%s\t%s\t%d" % (metric, key, value))
    return "\n".join(lines) + "\n"


def split_header(text):
    """The leading comment block of a baseline file, kept when it is rewritten."""
    head = []
    for line in text.splitlines():
        if line.startswith("#") or not line.strip():
            head.append(line)
        else:
            break
    return "\n".join(head)


def atomic_write(path, text):
    """Temp file in the same directory, then rename: an interrupted run never leaves a half-written baseline."""
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".baseline-")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


# --- measuring ------------------------------------------------------------------------------------------------

def is_ours(path):
    return path.startswith(OUR_DIRS) and path.endswith(SOURCE_EXT) and not any(p in "/" + path for p in SKIP_PARTS)


_FUNC_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(.+)@(\d+)-(\d+)@(.+)$")
_FILE_RE = re.compile(r"^\s*(\d+)\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+(\d+)\s+(\S.*\.(?:c|cpp|h|hpp))\s*$")
_SU_RE = re.compile(r"^(.*?):(\d+):(\d+):(.*)$")  # C++ names hold "::" and parentheses


def merge_max(dst, src):
    """Fold src into dst keeping the larger value: one source compiled into two objects (shared/ code, two build dirs)
    must not let the last .su file read hide the bigger frame."""
    for k, v in src.items():
        if v > dst.get(k, 0):
            dst[k] = v
        else:
            dst.setdefault(k, v)


def parse_lizard(text):
    """Parse lizard -ENS text output into {(metric, key): value}. Functions are keyed 'path:name'; further functions
    of the same name in one file (C++ overloads, static helpers under #ifdef) get '#2', '#3' in source order. The
    ordinal is stable across edits elsewhere in the file and needs no line numbers."""
    out = {}
    seen = {}
    listed = set()
    for line in text.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            _nloc, ccn, _tok, params, length, nest, name, first, last, path = m.groups()
            if (path, name, first, last) in listed:
                continue  # lizard prints the over-threshold functions a second time in its warnings table
            listed.add((path, name, first, last))
            base = "%s:%s" % (path, name.strip())
            seen[base] = seen.get(base, 0) + 1
            key = base if seen[base] == 1 else "%s#%d" % (base, seen[base])
            out[("len", key)] = int(length)
            out[("ccn", key)] = int(ccn)
            out[("nest", key)] = int(nest)
            out[("params", key)] = int(params)
            continue
        m = _FILE_RE.match(line)
        if m and "@" not in line:
            out[("nloc", m.group(3).strip())] = int(m.group(1))
    return out


def parse_stack_usage(text, root, base=None):
    """Lines of a .su file: 'path:line:col:function<TAB>bytes<TAB>static|dynamic|bounded'. Same function twice (an
    inline in a header, one source in two components) keeps the larger frame."""
    out = {}
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or not parts[1].isdigit():
            continue
        head = _SU_RE.match(parts[0])
        if not head:
            continue
        # gcc prints the path as given on the command line: relative to the build dir, or absolute
        path = os.path.relpath(os.path.join(base or root, head.group(1)), root)
        if not is_ours(path):
            continue
        merge_max(out, {("stack", "%s:%s" % (path, head.group(4))): int(parts[1])})
    return out


def _git(root, *args):
    return subprocess.run(["git", "-C", root] + list(args), check=True, capture_output=True, text=True).stdout


def tracked_sources(root):
    """Tracked and untracked-but-not-ignored files: a new file counts before it is `git add`ed."""
    names = set(_git(root, "ls-files").split("\n"))
    names.update(_git(root, "ls-files", "--others", "--exclude-standard").split("\n"))
    return sorted(f for f in names if is_ours(f))


def measure_lizard(root, files, lizard=("lizard",)):
    """Returns (measured, problems). A chunk where lizard fails, or a file without an nloc row, is a problem: the
    gate must not pass because less was measured than asked. -i lifts lizard's own exit-1-on-warnings."""
    out, problems = {}, []
    for i in range(0, len(files), 200):
        chunk = files[i : i + 200]
        try:
            res = subprocess.run(list(lizard) + ["-ENS", "-i", "1000000"] + chunk, cwd=root, capture_output=True, text=True)
        except OSError as e:
            return out, ["cannot run lizard: %s" % e]
        if res.returncode != 0:
            problems.append("lizard failed (exit %d) on %d files starting at %s: %s"
                            % (res.returncode, len(chunk), chunk[0], res.stderr.strip()[:300]))
            continue
        got = parse_lizard(res.stdout)
        out.update(got)
        problems.extend("lizard returned no result for %s" % f for f in chunk if ("nloc", f) not in got)
    return out, problems


def live_objects(bdir):
    """Set of object paths the build dir's compile_commands.json lists, or None when it is missing or unreadable.
    ninja never deletes the .obj/.su of a source that left the build (moved or dropped from SRCS), so the files on
    disk say nothing about the current build; the compile database is rewritten at every cmake run and does."""
    try:
        with open(os.path.join(bdir, "compile_commands.json")) as f:
            db = json.load(f)
    except (OSError, ValueError):
        return None
    return {os.path.realpath(os.path.join(e.get("directory", bdir), e["output"])) for e in db if e.get("output")}


def measure_stack(root, build_dirs):
    """Returns (measured, problems): every build dir must hold current .su files, else -fstack-usage is not active.
    A .su counts only when its object (`x.c.su` -> `x.c.obj`) is an output of the current compile database."""
    out, problems = {}, []
    for bdir in build_dirs:
        found, stale = 0, 0
        live = live_objects(bdir)
        if live is None:
            problems.append("no readable compile_commands.json in %s: cannot tell current .su files from stale ones" % bdir)
            continue
        for dirpath, _dirs, names in os.walk(bdir):
            for name in names:
                if not name.endswith(".su"):
                    continue
                path = os.path.join(dirpath, name)
                if os.path.realpath(path[: -len(".su")] + ".obj") not in live:
                    stale += 1
                    continue
                found += 1
                with open(path, errors="replace") as f:
                    merge_max(out, parse_stack_usage(f.read(), root, bdir))
        if stale:
            print("standard: ignored %d stale .su files under %s (source no longer built)" % (stale, bdir), file=sys.stderr)
        if not found:
            problems.append("no .su files under %s: build it first, and -fstack-usage must be on for the components" % bdir)
    return out, problems


def partition_size(csv_text, name):
    """Size of a partition-table row; accepts 0x hex, decimal and the K/M suffixes."""
    for line in csv_text.splitlines():
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[0] == name:
            size = cols[4]
            mult = {"K": 1024, "M": 1024 * 1024}.get(size[-1:].upper(), 1)
            return int(size.rstrip("kKmM"), 0) * mult
    raise GateError("partition %s not in the table" % name)


def parse_archive_sizes(csv_text):
    """esp_idf_size --archives --format csv: first two columns are the archive and its total size."""
    out = {}
    for row in csv.reader(io.StringIO(csv_text)):
        if len(row) >= 2 and row[1].isdigit():
            out[("size", os.path.basename(row[0]))] = int(row[1])
    return out


POOLS = "<merged-strings>"  # baseline key of the merged string/constant pools, never an archive name
# An input section of a SHF_MERGE pool: `.rodata.<fn>.str1.4`, `.srodata.cst8`, `.rodata.str4.4`.
POOL_SECTION = re.compile(r"^ (\.\S*\.(?:str\d+\.\d+|cst\d+))(?:\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.*\S))?\s*$")
POOL_ARCHIVE = re.compile(r"([^/()]+\.a)\(")


def parse_merged_pools(map_text):
    """{archive: bytes} of the merged pools in a link map. The linker merges every contributor into one blob and the
    map (and esp_idf_size) books all of it on the first contributor; the others show size 0 or address 0 (discarded)."""
    out = {}
    lines = map_text.split("\n")
    for i, line in enumerate(lines):
        m = POOL_SECTION.match(line)
        if not m:
            continue
        if m.group(2):
            addr, size, owner = m.group(2), m.group(3), m.group(4)
        else:  # long section name: the numbers are on the next line
            nxt = lines[i + 1].split(None, 2) if i + 1 < len(lines) else []
            if len(nxt) < 3:
                continue
            addr, size, owner = nxt
        arc = POOL_ARCHIVE.search(owner)
        if arc and int(addr, 16) and int(size, 16):
            out[arc.group(1)] = out.get(arc.group(1), 0) + int(size, 16)
    return out


def split_pools(archives, pools):
    """Archive sizes without the merged pools, plus the pool line, so moving the first contributor moves nothing."""
    out = dict(archives)
    for arc, size in pools.items():
        key = ("size", arc)
        if key in out:
            out[key] = max(0, out[key] - size)
    out[("size", POOLS)] = sum(pools.values())
    return out


def gather_size(root, build):
    """Returns (image_bytes, slot_bytes, archive sizes, problems)."""
    bin_path = os.path.join(build, "badge_fw.bin")
    map_path = os.path.join(build, "badge_fw.map")
    missing = [p for p in (bin_path, map_path) if not os.path.exists(p)]
    if missing:
        return 0, 0, {}, ["missing %s: the P4 image is not built" % ", ".join(missing)]
    with open(os.path.join(root, "firmware", "partitions.csv")) as f:
        slot = partition_size(f.read(), "ota_0")
    res = subprocess.run([sys.executable, "-m", "esp_idf_size", "--archives", "--format", "csv", map_path],
                         capture_output=True, text=True)
    if res.returncode != 0:
        return 0, 0, {}, ["esp_idf_size failed (exit %d): %s" % (res.returncode, res.stderr.strip()[-500:])]
    archives = parse_archive_sizes(res.stdout)
    if not archives:
        return 0, 0, {}, ["esp_idf_size gave no archive sizes: %s" % res.stdout.strip()[:200]]
    with open(map_path, errors="replace") as f:
        archives = split_pools(archives, parse_merged_pools(f.read()))
    return os.path.getsize(bin_path), slot, archives, []


def gather_c6_image(root):
    """Returns (image_bytes, slot_bytes, problems) of the C6 network adapter against its ota_0."""
    bin_path = os.path.join(root, "c6", "build", "network_adapter.bin")
    if not os.path.exists(bin_path):
        return 0, 0, ["missing %s: the C6 image is not built" % bin_path]
    with open(os.path.join(root, "c6", "partitions.esp32c6.csv")) as f:
        return os.path.getsize(bin_path), partition_size(f.read(), "ota_0"), []


# --- comparing ------------------------------------------------------------------------------------------------

def compare_static(measured, baseline, changed_files=None):
    """The ratchet. Returns (failures, warnings, stale) as lists of strings.

    failures: a value past the fail line that is not baselined, or is above its baselined value.
    warnings: a value past the warn line (not past fail) in a changed file (all files if changed_files is None).
    stale: baseline entries that are fixed, lowered or gone; the caller fails on them (run --shrink)."""
    failures, warnings, stale = [], [], []
    for (metric, key), value in sorted(measured.items()):
        if metric not in LIMITS:
            continue
        warn, fail = LIMITS[metric]
        base = baseline.get((metric, key))
        if value > fail:
            if base is None:
                failures.append("NEW   %s %s = %d (fail line %d)" % (metric, key, value, fail))
            elif value > base:
                failures.append("WORSE %s %s = %d (baseline %d, fail line %d)" % (metric, key, value, base, fail))
        elif value > warn:
            path = key.split(":")[0]
            if changed_files is None or path in changed_files:
                warnings.append("warn  %s %s = %d (warn line %d)" % (metric, key, value, warn))
    for (metric, key), base in sorted(baseline.items()):
        if metric not in LIMITS:
            continue
        cur = measured.get((metric, key))
        if cur is None:
            stale.append("%s %s: gone from the tree" % (metric, key))
        elif cur < base:
            below = "" if cur > LIMITS[metric][1] else " (below the fail line)"
            stale.append("%s %s: %d -> %d%s" % (metric, key, base, cur, below))
    return failures, warnings, stale


def check_shrink_only(baseline, base_baseline):
    """The 'may only shrink' rule against the merge-base version of the file. A static entry that is new or higher
    fails, unless a 'moved' record in the baseline says it is the old entry under a new name (and the old entry was at
    least as high). Returns failure strings."""
    out = []
    moved = {}
    for (metric, key) in baseline:
        if metric == MOVED and "=>" in key:
            head, new = key.split("=>", 1)
            m, old = head.split(":", 1) if ":" in head else ("", head)
            moved[(m, new)] = old
    for (metric, key), value in sorted(baseline.items()):
        if metric not in LIMITS:
            continue
        base = base_baseline.get((metric, key))
        if base is None:
            old = moved.get((metric, key))
            old_val = base_baseline.get((metric, old)) if old is not None else None
            if old_val is None or value > old_val:
                out.append("GREW  baseline %s %s = %d is not in the merge-base baseline (new entries are not allowed; "
                           "use --rename for a moved offender)" % (metric, key, value))
        elif value > base:
            out.append("GREW  baseline %s %s raised %d -> %d (a baseline only shrinks)" % (metric, key, base, value))
    return out


def compare_image(label, image_bytes, slot_bytes):
    pct = 100.0 * image_bytes / slot_bytes
    msg = "%s %d of %d B OTA slot = %.1f%%" % (label, image_bytes, slot_bytes, pct)
    if pct > IMAGE_PCT[1]:
        return ["FAIL  %s (fail line %.0f%%)" % (msg, IMAGE_PCT[1])], [], msg
    if pct > IMAGE_PCT[0]:
        return [], ["warn  %s (warn line %.0f%%)" % (msg, IMAGE_PCT[0])], msg
    return [], [], msg


def compare_sizes(measured, ref, acked=None):
    """Archive growth of the branch against REF, the sizes at the merge base with origin/main (#297). A new archive
    counts from zero. ACKED holds the working-tree sizes: growth past the fail line passes only when the file's value
    is at least the measured one, which only `--size-update` writes (a green build never records growth past the
    line). That is an honour rule, not a provenance check, so the accepted growth warns on every run."""
    failures, warnings = [], []
    for (metric, key), value in sorted(measured.items()):
        if metric != "size":
            continue
        base = ref.get(("size", key), 0)
        delta = value - base
        if delta > GROWTH[1]:
            if acked is not None and value <= acked.get(("size", key), -1):
                warnings.append("warn  size %s +%d B over the reference, accepted by the baseline line: needs Lucas's ok"
                                % (key, delta))
                continue
            failures.append("GREW  %s +%d B (%d -> %d, fail line +%d; --size-update if intended)"
                            % (key, delta, base, value, GROWTH[1]))
        elif delta > GROWTH[0]:
            warnings.append("warn  %s +%d B (%d -> %d, warn line +%d)" % (key, delta, base, value, GROWTH[0]))
    return failures, warnings


def rewrite_sizes(text, sizes):
    """TEXT with only the numbers of its `size` lines replaced by SIZES. Every other line (comments, blanks, static
    entries) stays byte for byte; a gone archive loses its line, a new one is inserted at its sorted position."""
    lines = text.splitlines()
    out, seen = [], set()
    for line in lines:
        parts = line.split("\t")
        if len(parts) == 3 and parts[0] == "size":
            if ("size", parts[1]) in sizes:
                seen.add(parts[1])
                out.append("size\t%s\t%d" % (parts[1], sizes[("size", parts[1])]))
            continue
        out.append(line)
    for key in sorted(k[1] for k in sizes if k[0] == "size" and k[1] not in seen):
        new = "size\t%s\t%d" % (key, sizes[("size", key)])
        at = next((n for n, l in enumerate(out) if l.startswith("size\t") and l.split("\t")[1] > key), None)
        if at is None:
            last = max((n for n, l in enumerate(out) if l.startswith("size\t")), default=None)
            at = len(out) if last is None else last + 1
        out.insert(at, new)
    return "\n".join(out) + "\n"


def shrink(measured, baseline, problems=()):
    """The baseline after a ratchet step: entries fixed or gone are dropped, entries that got smaller are lowered.
    It never adds an entry or raises one. Refuses when the measurement was incomplete (it would drop real offenders)."""
    if problems:
        raise GateError("refusing to shrink, the measurement is incomplete: " + "; ".join(problems))
    out = {}
    for (metric, key), base in baseline.items():
        if metric not in LIMITS:
            out[(metric, key)] = base
            continue
        cur = measured.get((metric, key))
        if cur is not None and cur > LIMITS[metric][1]:
            out[(metric, key)] = min(base, cur)
    return out


def apply_rename(baseline, old, new):
    """Move the static entries of OLD to NEW keeping their values. OLD is a file (renames its nloc entry and every
    'file:function' one) or a 'file:function' key. Each move leaves a 'moved' record that check_shrink_only accepts
    against the merge-base baseline. Returns the new baseline and the list of moves."""
    out, moves = dict(baseline), []
    for (metric, key), value in baseline.items():
        if metric not in LIMITS:
            continue
        if key == old or key.startswith(old + ":") or key.startswith(old + "#"):
            new_key = new + key[len(old):]
            del out[(metric, key)]
            out[(metric, new_key)] = value
            out[(MOVED, "%s:%s=>%s" % (metric, key, new_key))] = 0
            moves.append("%s %s -> %s = %d" % (metric, key, new_key, value))
    if not moves:
        raise GateError("no baseline entry for %s" % old)
    return out, moves


def drop_settled_moves(baseline, base_baseline):
    """'moved' records are only needed until the merge base has the new key."""
    out = dict(baseline)
    for (metric, key) in baseline:
        if metric == MOVED and "=>" in key:
            head, new = key.split("=>", 1)
            if (head.split(":", 1)[0], new) in base_baseline:
                del out[(metric, key)]
    return out


def initial_baseline(measured, sizes):
    out = {k: v for k, v in measured.items() if k[0] in LIMITS and v > LIMITS[k[0]][1]}
    out.update(sizes)
    return out


def diff_sizes(old, new):
    lines = []
    for key in sorted(set(old) | set(new)):
        if key[0] != "size":
            continue
        a, b = old.get(key), new.get(key)
        if a != b:
            lines.append("%s: %s -> %s%s" % (key[1], "new" if a is None else a, "gone" if b is None else b,
                                             "" if a is None or b is None else " (%+d)" % (b - a)))
    return lines


# --- main -----------------------------------------------------------------------------------------------------

def merge_base(root):
    for ref in ("origin/main", "main"):
        r = subprocess.run(["git", "-C", root, "merge-base", "HEAD", ref], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return ""


def changed_since_base(root):
    base = merge_base(root)
    names = set(_git(root, "ls-files", "--others", "--exclude-standard").split())
    if base:
        names.update(_git(root, "diff", "--name-only", "--diff-filter=d", base).split())
    return names


def base_baseline_text(root):
    """The baseline at the merge base (HEAD when there is none). None when the file does not exist there yet: the
    change that introduces the baseline has nothing to be compared with."""
    ref = merge_base(root) or "HEAD"
    r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (ref, BASELINE_REL)], capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


REMOTE_MAIN = "refs/remotes/origin/main"


def reference_sizes(root):
    """The size lines at the merge base of HEAD and origin/main: what this branch (in main: what the next push) is
    measured against, however many commits or rebuilds it holds. Fails closed: an unreadable reference is an error,
    never a silent comparison with the working file (`--no-ref` is the explicit way past, for a repo without one)."""
    def git(*args):
        try:
            r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True)
        except OSError as e:
            raise GateError("cannot run git for the size reference: %s (--no-ref skips it on purpose)" % e)
        if r.returncode != 0:
            raise GateError("size reference: `git %s` failed: %s (fetch origin, or --no-ref to skip on purpose)"
                            % (" ".join(args), r.stderr.strip()[:200]))
        return r.stdout
    base = git("merge-base", "HEAD", REMOTE_MAIN).strip()
    return parse_baseline(git("show", "%s:%s" % (base, BASELINE_REL)))


def measure_all(root):
    """Everything the gate looks at. Returns (measured, sizes, size_info, problems)."""
    measured, problems = measure_lizard(root, tracked_sources(root))
    stack, stack_problems = measure_stack(root, [os.path.join(root, "firmware", "build"), os.path.join(root, "c6", "build")])
    measured.update(stack)
    problems += stack_problems
    image, slot, sizes, size_problems = gather_size(root, os.path.join(root, "firmware", "build"))
    c6_image, c6_slot, c6_problems = gather_c6_image(root)
    problems += size_problems + c6_problems
    return measured, sizes, {"p4": (image, slot), "c6": (c6_image, c6_slot)}, problems


def run_check(root, baseline, base_baseline, measured, sizes, size_info, problems, changed, ref=None):
    """The gate proper. Returns (failures, warnings, notes)."""
    fails = ["INCOMPLETE " + p for p in problems]
    warns, notes = [], []
    f, w, stale = compare_static(measured, baseline, changed)
    fails += f
    warns += w
    fails += ["STALE baseline %s (run: python3 -I tools/check_standard.py --shrink)" % s for s in stale]
    if base_baseline is not None:
        fails += check_shrink_only(baseline, base_baseline)
    else:
        notes.append("no baseline at the merge base: this change introduces it, shrink-only check skipped")
    for label, key in (("P4 app image", "p4"), ("C6 image", "c6")):
        image, slot = size_info[key]
        if slot:
            f, w, msg = compare_image(label, image, slot)
            fails += f
            warns += w
            notes.append(msg)
    f, w = compare_sizes(sizes, baseline if ref is None else ref, baseline)
    fails += f
    warns += w
    return fails, warns, notes


def main(argv, root=None, measure=None):
    root = root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    measure = measure or measure_all
    no_ref = "--no-ref" in argv
    argv = [a for a in argv if a != "--no-ref"]
    mode = argv[1] if len(argv) > 1 else ""
    base_path = os.path.join(root, *BASELINE_REL.split("/"))
    exists = os.path.exists(base_path)
    text = ""
    if exists:
        with open(base_path) as f:
            text = f.read()
    baseline = parse_baseline(text)

    if mode == "--init":
        if exists:
            raise GateError("%s exists; --init only creates the first baseline" % BASELINE_REL)
    if mode == "--rename":
        if len(argv) != 4:
            raise GateError("usage: check_standard.py --rename OLD NEW (file path, or file:function)")
        new, moves = apply_rename(baseline, argv[2], argv[3])
        atomic_write(base_path, format_baseline(split_header(text), new))
        print("\n".join(moves))
        return 0

    measured, sizes, size_info, problems = measure(root)
    if mode == "--init":
        if problems:
            raise GateError("refusing to create a baseline from an incomplete measurement: " + "; ".join(problems))
        atomic_write(base_path, format_baseline(DEFAULT_HEADER, initial_baseline(measured, sizes)))
        return 0
    if mode == "--shrink":
        new = shrink(measured, baseline, problems)
        base_text = base_baseline_text(root)
        if base_text is not None:
            new = drop_settled_moves(new, parse_baseline(base_text))
        atomic_write(base_path, format_baseline(split_header(text), new))
        return 0
    if mode == "--size-update":
        if problems:
            raise GateError("refusing to update sizes, the measurement is incomplete: " + "; ".join(problems))
        new_text = rewrite_sizes(text, sizes)
        for line in diff_sizes(baseline, parse_baseline(new_text)) or ["no size changed"]:
            print("size baseline: " + line)
        atomic_write(base_path, new_text)
        return 0
    if mode == "--report":
        for p in problems:
            print("note: " + p, file=sys.stderr)
        for (metric, key), value in sorted(measured.items(), key=lambda kv: (kv[0][0], -kv[1])):
            if value > LIMITS[metric][0]:
                print("%-6s %6d  %s%s" % (metric, value, key, "  FAIL" if value > LIMITS[metric][1] else ""))
        return 0
    if mode:
        raise GateError("unknown option %s" % mode)

    if no_ref:
        print("standard: --no-ref: sizes are compared with the working-tree baseline, not with origin/main", file=sys.stderr)
        ref = baseline
    else:
        ref = reference_sizes(root)
    base_text = base_baseline_text(root)
    fails, warns, notes = run_check(root, baseline, parse_baseline(base_text) if base_text is not None else None,
                                    measured, sizes, size_info, problems, changed_since_base(root),
                                    ref)
    for line in notes:
        print("standard: " + line)
    for line in warns:
        print(line)
    for line in fails:
        print(line, file=sys.stderr)
    if fails:
        print("standard: %d problem(s). Fix the code; a baseline only shrinks, see docs/code-standard.md" % len(fails),
              file=sys.stderr)
        return 1
    recorded = rewrite_sizes(text, sizes)
    if recorded != text:
        atomic_write(base_path, recorded)
        print("standard: recorded the measured sizes in %s (commit it with the change)" % BASELINE_REL)
    print("standard: ok (%d metrics measured, %d warnings in changed files)" % (len(measured), len(warns)))
    return 0


if __name__ == "__main__":
    if shutil.which("lizard") is None and "--rename" not in sys.argv:
        print("standard: lizard is not installed (pipx install lizard); the gate cannot run", file=sys.stderr)
        sys.exit(1)
    try:
        sys.exit(main(sys.argv))
    except GateError as e:
        print("standard: %s" % e, file=sys.stderr)
        sys.exit(1)
