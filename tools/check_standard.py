#!/usr/bin/env python3
"""Code and resource standard gate (#202). Limits and rationale: docs/code-standard.md.

Run by firmware/build.sh build (through tools/check_standard.sh) after the P4 image is built. Run it with
`python3 -I tools/check_standard.py [--report | --shrink | --size-update]`.

Static metrics (lizard: lines per file, lines/CCN/nesting/parameters per function; -fstack-usage: stack frame per
function) are ratcheted: a value past the fail line fails unless tools/standard-baseline.txt lists it at that value or
more, so existing offenders stay and may only shrink. Warn-line hits print for the files changed since the merge base.
Size: the app image against its OTA slot, and the size of every linked archive against the baseline (growth in one
change)."""
import csv
import io
import os
import re
import subprocess
import sys

# metric: (warn, fail, unit). A value above warn warns, above fail fails.
LIMITS = {
    "nloc": (800, 1500),
    "len": (60, 150),
    "ccn": (15, 25),
    "nest": (4, 6),
    "params": (6, 8),
    "stack": (512, 1024),
}
IMAGE_PCT = (85.0, 92.0)  # app image / OTA slot
GROWTH = (2048, 8192)  # bytes one archive may grow past its baseline
OUR_DIRS = ("firmware/components/", "firmware/main/", "c6/main/", "shared/")
SKIP_PARTS = ("/thirdparty/", "/managed_components/", "/build/")
SOURCE_EXT = (".c", ".cpp", ".h", ".hpp")


# --- baseline ---------------------------------------------------------------------------------------------------

def parse_baseline(text):
    """Lines 'metric<TAB>key<TAB>value'; '#' lines and blanks are comments. Returns {(metric, key): int}."""
    out = {}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3 or not parts[2].strip().isdigit():
            raise ValueError("baseline line %d is not 'metric<TAB>key<TAB>value': %r" % (n, line))
        out[(parts[0], parts[1])] = int(parts[2])
    return out


def format_baseline(header, entries):
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


# --- measuring ------------------------------------------------------------------------------------------------

def is_ours(path):
    return path.startswith(OUR_DIRS) and path.endswith(SOURCE_EXT) and not any(p in "/" + path for p in SKIP_PARTS)


_FUNC_RE = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(.+)@(\d+)-(\d+)@(.+)$")
_FILE_RE = re.compile(r"^\s*(\d+)\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+(\d+)\s+(\S.*\.(?:c|cpp|h|hpp))\s*$")


def parse_lizard(text):
    """Parse lizard -ENS text output into {(metric, key): value}. Functions are keyed 'path:name'; a name used twice
    in one file (static helpers in different #ifdefs) keeps the worst value."""
    out = {}

    def put(metric, key, value):
        out[(metric, key)] = max(value, out.get((metric, key), 0))

    for line in text.splitlines():
        m = _FUNC_RE.match(line)
        if m:
            nloc, ccn, _tok, params, length, nest, name, _a, _b, path = m.groups()
            key = "%s:%s" % (path, name.strip())
            put("len", key, int(length))
            put("ccn", key, int(ccn))
            put("nest", key, int(nest))
            put("params", key, int(params))
            continue
        m = _FILE_RE.match(line)
        if m and "@" not in line:
            put("nloc", m.group(3).strip(), int(m.group(1)))
    return out


_SU_RE = re.compile(r"^(.*?):(\d+):(\d+):(.*)$")  # C++ names hold "::" and parentheses


def parse_stack_usage(text, root, base=None):
    """Lines of a .su file: 'path:line:col:function<TAB>bytes<TAB>static|dynamic|bounded'."""
    out = {}
    for line in text.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or not parts[1].isdigit():
            continue
        head = _SU_RE.match(parts[0])
        if not head:
            continue
        head = head.groups()
        # gcc prints the path as given on the command line: relative to the build dir, or absolute
        path = os.path.relpath(os.path.join(base or root, head[0]), root)
        if not is_ours(path):
            continue
        key = "%s:%s" % (path, head[3])
        out[("stack", key)] = max(int(parts[1]), out.get(("stack", key), 0))
    return out


def tracked_sources(root):
    files = subprocess.run(["git", "-C", root, "ls-files"], check=True, capture_output=True, text=True).stdout.split("\n")
    return [f for f in files if is_ours(f)]


def measure_lizard(root, files):
    # Chunked so the command line stays short; the file table needs whole files, which chunks keep
    out = {}
    for i in range(0, len(files), 200):
        res = subprocess.run(["lizard", "-ENS"] + files[i : i + 200], cwd=root, capture_output=True, text=True)
        out.update(parse_lizard(res.stdout))
    return out


def measure_stack(root, build_dirs):
    out = {}
    found = 0
    for bdir in build_dirs:
        for dirpath, _dirs, names in os.walk(bdir):
            for name in names:
                if name.endswith(".su"):
                    found += 1
                    with open(os.path.join(dirpath, name), errors="replace") as f:
                        out.update(parse_stack_usage(f.read(), root, bdir))
    return out, found


def partition_size(csv_text, name):
    for line in csv_text.splitlines():
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[0] == name:
            return int(cols[4], 0)
    raise ValueError("partition %s not in the table" % name)


def parse_archive_sizes(csv_text):
    """esp_idf_size --archives --format csv: first two columns are the archive and its total size."""
    out = {}
    for row in csv.reader(io.StringIO(csv_text)):
        if len(row) >= 2 and row[1].isdigit():
            out[("size", os.path.basename(row[0]))] = int(row[1])
    return out


# --- comparing ------------------------------------------------------------------------------------------------

def compare_static(measured, baseline, changed_files=None):
    """The ratchet. Returns (failures, warnings, stale) as lists of strings.

    failures: a value past the fail line that is not baselined, or is above its baselined value.
    warnings: a value past the warn line (not past fail) in a changed file (all files if changed_files is None).
    stale: baseline entries that are fixed or gone; they can be dropped with --shrink."""
    failures, warnings, stale = [], [], []
    for (metric, key), value in sorted(measured.items()):
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
        if metric == "size":
            continue
        cur = measured.get((metric, key))
        if cur is None:
            stale.append("%s %s: gone" % (metric, key))
        elif cur < base:
            stale.append("%s %s: %d -> %d%s" % (metric, key, base, cur, "" if cur > LIMITS[metric][1] else " (below the fail line)"))
    return failures, warnings, stale


def compare_image(image_bytes, slot_bytes):
    pct = 100.0 * image_bytes / slot_bytes
    msg = "app image %d of %d B OTA slot = %.1f%%" % (image_bytes, slot_bytes, pct)
    if pct > IMAGE_PCT[1]:
        return ["FAIL  %s (fail line %.0f%%)" % (msg, IMAGE_PCT[1])], [], msg
    if pct > IMAGE_PCT[0]:
        return [], ["warn  %s (warn line %.0f%%)" % (msg, IMAGE_PCT[0])], msg
    return [], [], msg


def compare_sizes(measured, baseline):
    """Archive growth since the baseline. A new archive counts from zero."""
    failures, warnings, stale = [], [], []
    for (metric, key), value in sorted(measured.items()):
        if metric != "size":
            continue
        base = baseline.get(("size", key), 0)
        delta = value - base
        if delta > GROWTH[1]:
            failures.append("GREW  %s +%d B (%d -> %d, fail line +%d)" % (key, delta, base, value, GROWTH[1]))
        elif delta > GROWTH[0]:
            warnings.append("warn  %s +%d B (%d -> %d, warn line +%d)" % (key, delta, base, value, GROWTH[0]))
        elif -delta > GROWTH[1]:
            stale.append("size %s shrank by %d B, update the baseline" % (key, -delta))
    return failures, warnings, stale


def shrink(measured, baseline):
    """The baseline after a ratchet step: entries fixed or gone are dropped, entries that got smaller are lowered.
    It never adds an entry or raises one."""
    out = {}
    for (metric, key), base in baseline.items():
        if metric == "size":
            out[(metric, key)] = base
            continue
        cur = measured.get((metric, key))
        if cur is not None and cur > LIMITS[metric][1]:
            out[(metric, key)] = min(base, cur)
    return out


def initial_baseline(measured, sizes):
    out = {k: v for k, v in measured.items() if v > LIMITS[k[0]][1]}
    out.update(sizes)
    return out


# --- main -----------------------------------------------------------------------------------------------------

def changed_since_base(root):
    def git(*a):
        return subprocess.run(["git", "-C", root] + list(a), capture_output=True, text=True)

    base = ""
    for ref in ("origin/main", "main"):
        r = git("merge-base", "HEAD", ref)
        if r.returncode == 0 and r.stdout.strip():
            base = r.stdout.strip()
            break
    names = set()
    if base:
        names.update(git("diff", "--name-only", "--diff-filter=d", base).stdout.split())
    names.update(git("ls-files", "--others", "--exclude-standard").stdout.split())
    return names


def gather_size(root, build):
    bin_path = os.path.join(build, "badge_fw.bin")
    map_path = os.path.join(build, "badge_fw.map")
    if not (os.path.exists(bin_path) and os.path.exists(map_path)):
        return None
    with open(os.path.join(root, "firmware", "partitions.csv")) as f:
        slot = partition_size(f.read(), "ota_0")
    res = subprocess.run([sys.executable, "-m", "esp_idf_size", "--archives", "--format", "csv", map_path],
                         capture_output=True, text=True)
    if res.returncode != 0:
        return None
    return os.path.getsize(bin_path), slot, parse_archive_sizes(res.stdout)


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    mode = argv[1] if len(argv) > 1 else ""
    base_path = os.path.join(root, "tools", "standard-baseline.txt")
    text = open(base_path).read() if os.path.exists(base_path) else ""
    baseline = parse_baseline(text)

    measured = measure_lizard(root, tracked_sources(root))
    if not measured:
        print("standard: lizard found nothing to measure", file=sys.stderr)
        return 1
    stack, su_files = measure_stack(root, [os.path.join(root, "firmware", "build"), os.path.join(root, "c6", "build")])
    measured.update(stack)
    size = gather_size(root, os.path.join(root, "firmware", "build"))
    sizes = size[2] if size else {}

    if mode == "--init":
        open(base_path, "w").write(format_baseline(split_header(text) or "# standard baseline", initial_baseline(measured, sizes)))
        return 0
    if mode == "--shrink":
        new = shrink(measured, baseline)
        new.update({k: v for k, v in baseline.items() if k[0] == "size"})
        open(base_path, "w").write(format_baseline(split_header(text), new))
        return 0
    if mode == "--size-update":
        new = {k: v for k, v in baseline.items() if k[0] != "size"}
        new.update(sizes)
        open(base_path, "w").write(format_baseline(split_header(text), new))
        return 0
    if mode == "--report":
        for (metric, key), value in sorted(measured.items(), key=lambda kv: (kv[0][0], -kv[1])):
            if value > LIMITS[metric][0]:
                print("%-6s %6d  %s%s" % (metric, value, key, "  FAIL" if value > LIMITS[metric][1] else ""))
        return 0

    changed = changed_since_base(root)
    fails, warns, stale = compare_static(measured, baseline, changed)
    if size:
        f2, w2, msg = compare_image(size[0], size[1])
        f3, w3, s3 = compare_sizes(sizes, baseline)
        fails += f2 + f3
        warns += w2 + w3
        stale += s3
        print("standard: " + msg)
    else:
        print("note: no linked image in firmware/build, size checks skipped", file=sys.stderr)
    if not su_files:
        print("note: no .su files (stack frames) in the build dirs, stack checks skipped", file=sys.stderr)
    for line in warns:
        print(line)
    for line in stale:
        print("stale baseline: " + line + " (tools/check_standard.py --shrink)")
    for line in fails:
        print(line, file=sys.stderr)
    if fails:
        print("standard: %d violation(s). Fix the code; a baseline entry is only ever lowered, see docs/code-standard.md"
              % len(fails), file=sys.stderr)
        return 1
    print("standard: ok (%d metrics measured, %d warnings in changed files, %d stale baseline entries)"
          % (len(measured), len(warns), len(stale)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
