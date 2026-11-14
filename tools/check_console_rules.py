#!/usr/bin/env python3
"""Console rule guard (#464): every sub-command row (console_sub_t, fw_console/console_sub.h) that transmits
(CON_F_TX) or restarts (CON_F_RESTART) and has origin CON_ORIGIN_ANY fails the build, unless it is on ALLOW below.

Why: run_command checks each row's gate and origin once, so a row is the one place that says who may make the badge
transmit or restart. A new transmitting or restarting form that anyone (Wi-Fi, the Console app) may run must be a
decision, made here with its reason, not a default. Whether each entry on ALLOW stays open is the owner's call (#464).

The check reads the sources: every `console_sub_t const NAME[...] = {...};` (or mesh_sub_t) table under firmware/,
with the plain #defines of the same file expanded and CON_DEV_WORD("x") read as "x" (the developer image, the larger
one). A row it cannot read fails, and so does an ALLOW entry that matches no row (it would hide the next one).
Usage: check_console_rules.py [--selftest] [--list]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

# (table, word): word as the row has it, "*" for a REST row (any arguments). Each line says why it stays open.
ALLOW = {
    # fw_console: tools/badge.sh restarts the badge over Wi-Fi; "slot other" is refused while the USB hold is on
    ("OS_REBOOT_RULES", "*"),
    ("OS_SLOT_RULES", "other"),
    # developer-mode test arms that crash the badge on purpose (crash, app crash): the bench runs them remotely
    ("CRASH_RULES", "test panic"),
    ("CRASH_RULES", "test wdt"),
    ("APP_RULES", "crash "),
    # MeshCore's sends: the on-air allow list (#289) and the bench console scope (#290) judge them in the senders
    ("MESH_SUBS", "send "),
    ("MESH_SUBS", "dm "),
    ("MESH_SUBS", "trace"),
    ("MESH_SUBS", "discover "),
    ("MESH_SUBS", "telemetry"),
    ("MESH_SUBS", "history"),
    ("MESH_SUBS", "remote"),
    ("MESH_SUBS", "advert"),
    ("MESH_ADVERT_RULES", "*"),
    # no origin rule before #464 (the C6 resets and reflash, the LoRa switch): kept as they were, for the owner to decide
    ("C6_RULES", "reset"),
    ("C6_RULES", "reset rom"),
    ("C6_RULES", "reattach"),
    ("C6_RULES", "verify"),
    ("C6_RULES", "reflash"),
    ("LORA_RULES", "on"),
}

TABLE = re.compile(r"\b(?:console_sub_t|mesh_sub_t)\s+const\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{(.*?)\};", re.S)
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+?)[ \t]*$", re.M)
ROW = re.compile(r"(?:\[\s*\w+\s*\]\s*=\s*)?\{([^{}]*)\}")
WATCHED = ("CON_F_TX", "CON_F_RESTART")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def expand(text):
    """The file's plain object-like #defines, applied to the text after them (one level is enough for the tables)."""
    defs = {}
    for m in DEFINE.finditer(text):
        name, value = m.group(1), m.group(2)
        if "(" not in name:
            defs[name] = value
    out = text
    if defs:
        pat = re.compile(r"\b(" + "|".join(map(re.escape, defs)) + r")\b")
        out = pat.sub(lambda m: defs[m.group(1)], DEFINE.sub("", text))
    return re.sub(r"\bCON_DEV_WORD\s*\(\s*(\"[^\"]*\")\s*\)", r"\1", out)


def rows_of(body):
    """(word, match, origin, flags) per row; word is None for a row with NULL."""
    for m in ROW.finditer(body):
        fields = [f.strip() for f in m.group(1).split(",")]
        if fields == ["NULL", "CON_MATCH_END", "CON_GATE_OPEN", "CON_ORIGIN_ANY", "0"]:
            continue
        if len(fields) != 5:
            raise ValueError("row with %d fields: {%s}" % (len(fields), m.group(1).strip()))
        word, match, _gate, origin, flags = fields
        if word == "NULL":
            word = None
        elif re.fullmatch(r'"[^"]*"', word):
            word = word[1:-1]
        else:
            raise ValueError("row word is not a string literal or NULL: %s" % word)
        if not origin.startswith("CON_ORIGIN_"):
            raise ValueError("row origin is not a CON_ORIGIN_*: %s" % origin)
        yield word, match, origin, flags
    if "CONSOLE_SUB_END" not in body:
        raise ValueError("table without CONSOLE_SUB_END")


def tables(text):
    """(table name, rows) of every table in one source text."""
    clean = expand(strip_comments(text))
    clean = clean.replace("CONSOLE_SUB_END", "CONSOLE_SUB_END {NULL, CON_MATCH_END, CON_GATE_OPEN, CON_ORIGIN_ANY, 0}")
    for m in TABLE.finditer(clean):
        yield m.group(1), list(rows_of(m.group(2)))


def check(sources, allow):
    """(problems, open rows) over (path, text) pairs: the watched rows open to any origin that ALLOW does not name."""
    problems, open_rows, used = [], [], set()
    for path, text in sources:
        try:
            found = list(tables(text))
        except ValueError as e:
            problems.append("%s: %s" % (path, e))
            continue
        for name, rows in found:
            for word, match, origin, flags in rows:
                watched = [w for w in WATCHED if re.search(r"\b%s\b" % w, flags)]
                if not watched or origin != "CON_ORIGIN_ANY" or (word is None and match != "CON_MATCH_REST"):
                    continue  # a row with no word matches nothing in this image unless it is the REST row
                key = (name, "*" if match == "CON_MATCH_REST" else word)
                open_rows.append((path, key, "|".join(watched)))
                if key in allow:
                    used.add(key)
                else:
                    problems.append("%s: %s row %r %s with origin ANY: give it an origin rule, or put it on ALLOW "
                                    "in tools/check_console_rules.py with the reason" % (path, name, key[1],
                                                                                        " and ".join(watched)))
    for key in sorted(allow - used):
        problems.append("ALLOW entry %s %r matches no transmitting or restarting row: remove it" % key)
    return problems, open_rows


def sources():
    for sub in ("components", "main"):
        for p in sorted((FW / sub).rglob("*")):
            if p.suffix in (".c", ".cpp", ".h") and "build" not in p.relative_to(FW).parts:
                text = p.read_text(errors="replace")
                if "console_sub_t" in text or "mesh_sub_t" in text:
                    yield p.relative_to(ROOT), text


def selftest():
    src = """
#define TX CON_F_TX
#define OPEN CON_GATE_OPEN, CON_ORIGIN_ANY
static console_sub_t const A_RULES[] = {
    {"open", CON_MATCH_EXACT, OPEN, TX},                                   // flagged, allowed
    {"usb", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_TX},    // has a rule
    {CON_DEV_WORD("dev"), CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_RESTART}, // flagged, not allowed
    {NULL, CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_TX},      // compiled out: matches nothing
    {"quiet", CON_MATCH_WORD, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},           // not watched
    CONSOLE_SUB_END,
};
mesh_sub_t const M[N + 1] = {
    [X] = {NULL, CON_MATCH_REST, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART | 0x80}, // REST: "*"
    [N] = CONSOLE_SUB_END,
};
"""
    problems, open_rows = check([("t.c", src)], {("A_RULES", "open"), ("M", "*"), ("GONE", "x")})
    keys = sorted(k for _, k, _ in open_rows)
    assert keys == [("A_RULES", "dev"), ("A_RULES", "open"), ("M", "*")], keys
    assert len(problems) == 2, problems
    assert "A_RULES row 'dev'" in problems[0], problems
    assert "GONE" in problems[1], problems
    bad = 'static console_sub_t const B[] = {{"x", CON_MATCH_EXACT, CON_GATE_OPEN, CON_F_TX}, CONSOLE_SUB_END};'
    problems, _ = check([("b.c", bad)], set())
    assert len(problems) == 1 and "4 fields" in problems[0], problems
    devword = 'static console_sub_t const D[] = {{CON_DEV_WORD("w"), CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_TX}, CONSOLE_SUB_END};'
    problems, _ = check([("d.c", devword)], set())  # no #define in the file: CON_DEV_WORD is still read
    assert len(problems) == 1 and "D row 'w'" in problems[0], problems
    noend = 'static console_sub_t const C[] = {{"x", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0}};'
    problems, _ = check([("c.c", noend)], set())
    assert len(problems) == 1 and "CONSOLE_SUB_END" in problems[0], problems
    print("check_console_rules selftest ok")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    problems, open_rows = check(sources(), ALLOW)
    if "--list" in sys.argv:
        for path, (name, word), what in open_rows:
            print("%-14s %-18s %-16s %s" % (what, name, word, path))
    for p in problems:
        print("check_console_rules: " + p)
    print("console rules: %d transmitting or restarting rows open to any origin, %d on the allow list" %
          (len(open_rows), len(ALLOW)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
