#!/usr/bin/env python3
"""Console rule guard (#464), two checks over the firmware sources:

1. Every sub-command row (console_sub_t, fw_console/console_sub.h) that transmits (CON_F_TX) or restarts
   (CON_F_RESTART) and has origin CON_ORIGIN_ANY fails, unless it is on ALLOW below with its reason. run_command checks
   each row once, so a row is the one place that says who may make the badge transmit or restart: a form anyone (Wi-Fi,
   the Console app) may run must be a decision, not a default. Whether each ALLOW entry stays open is the owner's call.
2. The tables the firmware registers (console_register_rules / console_register_app_rules) are exactly the pairs the
   host pin tests (host_tests/console_pin_test.c RULES[]): a command registered with another table, without its table,
   or both with and without one fails, so the pin's verdicts are the badge's.

Fail closed: a console_sub_t or mesh_sub_t array the parser cannot read (either qualifier order is read), a row or a
field it does not recognise (a macro-built row, a flag from another header's macro), a registration whose name or
table it cannot resolve, and an ALLOW entry that matches no row (it would hide the next one).
Rows are read with the plain #defines of their own file expanded and CON_DEV_WORD("x") as "x" (the developer image).
Usage: check_console_rules.py [--selftest] [--list]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"
PIN = ROOT / "host_tests" / "console_pin_test.c"

# (table, word): word as the row has it, "*" for a REST row (any arguments). Each group says why it stays open.
ALLOW = {
    # fw_console: tools/badge.sh restarts the badge over Wi-Fi; "slot other" is refused while the USB hold is on
    ("OS_REBOOT_RULES", "*"),
    ("OS_SLOT_RULES", "other"),
    # developer-mode test arms that crash, restart or power off the badge on purpose: the bench runs them remotely
    ("CRASH_RULES", "test panic"),
    ("CRASH_RULES", "test wdt"),
    ("APP_RULES", "crash "),
    ("PSRAM_RULES", "speed"),
    ("THERMAL_RULES", "test"),
    # MeshCore's sends and its start (it claims the radio and adverts): the on-air allow list (#289) and the bench
    # console scope (#290) judge the sends in the senders
    ("MESH_SUBS", "start"),
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

WATCHED = ("CON_F_TX", "CON_F_RESTART")
MATCHES = {"CON_MATCH_EXACT", "CON_MATCH_PREFIX", "CON_MATCH_WORD", "CON_MATCH_WORD_RAW", "CON_MATCH_REST",
           "MESH_SUB_EXACT", "MESH_SUB_PREFIX", "MESH_SUB_WORD", "MESH_SUB_WORD_RAW"}
GATES = {"CON_GATE_OPEN", "CON_GATE_DEV"}
ORIGINS = {"CON_ORIGIN_ANY", "CON_ORIGIN_LOCAL", "CON_ORIGIN_UART"}
FLAG_TOKEN = re.compile(r"CON_F_(TX|RESTART|ERASE|USB_HOLD|SECRET|DEV_HINT|OWNER)|MESH_SUB_RUNNING|0|0x[0-9a-fA-F]+")
END = "CONSOLE_SUB_END"

# A table definition, either qualifier order (static const console_sub_t X[] = / console_sub_t const X[N] =)
DECL = re.compile(r"\b(?:console_sub_t|mesh_sub_t)\b(?:\s+const)?\s+(\w+)\s*\[[^\]]*\]\s*=\s*\{")
# Any array of the type with an initializer, however spelled: every one must be a DECL the parser read
LOOSE = re.compile(r"\b(?:console_sub_t|mesh_sub_t)\b[^;{}()=]*\[[^\]]*\]\s*=")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+?)[ \t]*$", re.M)
CALL = re.compile(r"(?<![\w])console_register(_app|_dev)?(_rules)?\s*\(")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def expand(text):
    """The file's plain object-like #defines applied to the text (one level is enough for the tables)."""
    defs = {m.group(1): m.group(2) for m in DEFINE.finditer(text)}
    out = text
    if defs:
        pat = re.compile(r"\b(" + "|".join(map(re.escape, defs)) + r")\b")
        out = pat.sub(lambda m: defs[m.group(1)], DEFINE.sub("", text))
    return re.sub(r"\bCON_DEV_WORD\s*\(\s*(\"[^\"]*\")\s*\)", r"\1", out)


def closing(text, at, open_ch, close_ch):
    """Index of the bracket that closes the one at text[at], skipping string literals."""
    depth, i = 0, at
    while i < len(text):
        c = text[i]
        if c == '"':
            i = text.index('"', i + 1) if '"' in text[i + 1:] else len(text)
            while text[i - 1] == "\\":
                i = text.index('"', i + 1)
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced %s" % open_ch)


def split_top(body):
    """body split at the commas outside brackets and string literals."""
    items, depth, cur, i = [], 0, [], 0
    while i < len(body):
        c = body[i]
        if c == '"':
            j = i + 1
            while body[j] != '"' or body[j - 1] == "\\":
                j += 1
            cur.append(body[i:j + 1])
            i = j + 1
            continue
        if c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
        if c == "," and depth == 0:
            items.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        items.append("".join(cur).strip())
    return items


def parse_row(item):
    """(word, match, gate, origin, flags) of one table item, None for CONSOLE_SUB_END; ValueError when unreadable."""
    item = re.sub(r"^\[\s*\w+\s*\]\s*=\s*", "", item)
    if item == END:
        return None
    if not (item.startswith("{") and item.endswith("}")):
        raise ValueError("a row the check cannot read: %s" % item)
    fields = split_top(item[1:-1])
    if len(fields) != 5:
        raise ValueError("row with %d fields: %s" % (len(fields), item))
    word, match, gate, origin, flags = fields
    if word == "NULL":
        word = None
    elif re.fullmatch(r'"[^"]*"', word):
        word = word[1:-1]
    else:
        raise ValueError("row word is not a string literal or NULL: %s" % word)
    for value, known, what in ((match, MATCHES, "match"), (gate, GATES, "gate"), (origin, ORIGINS, "origin")):
        if value not in known:
            raise ValueError("row %s is not one the check knows: %s" % (what, value))
    for tok in re.split(r"\s*\|\s*", flags.strip("() ")):
        if not FLAG_TOKEN.fullmatch(tok):
            raise ValueError("row flag is not one the check knows: %s" % tok)
    return word, match, gate, origin, flags


def tables(text):
    """(table name, rows) of every table in one source text; ValueError for one it cannot read."""
    clean = expand(strip_comments(text))
    decls = list(DECL.finditer(clean))
    if len(LOOSE.findall(clean)) != len(decls):
        raise ValueError("a console_sub_t array the check cannot read (%d declared, %d read)" %
                         (len(LOOSE.findall(clean)), len(decls)))
    for m in decls:
        body = clean[m.end() - 1:closing(clean, m.end() - 1, "{", "}") + 1]
        items = split_top(body[1:-1])
        if END not in [re.sub(r"^\[\s*\w+\s*\]\s*=\s*", "", it) for it in items]:
            raise ValueError("table %s without %s" % (m.group(1), END))
        yield m.group(1), [r for r in (parse_row(it) for it in items) if r]


def check_rows(sources, allow):
    """(problems, open rows): the watched rows open to any origin, those ALLOW does not name, and unreadable tables."""
    problems, open_rows, used = [], [], set()
    for path, text in sources:
        try:
            found = list(tables(text))
        except ValueError as e:
            problems.append("%s: %s" % (path, e))
            continue
        for name, rows in found:
            for word, match, _gate, origin, flags in rows:
                watched = [w for w in WATCHED if re.search(r"\b%s\b" % w, flags)]
                rest = match == "CON_MATCH_REST"
                if not watched or origin != "CON_ORIGIN_ANY" or (word is None and not rest):
                    continue  # a row with no word matches nothing in this image, unless it is the REST row
                key = (name, "*" if rest else word)
                open_rows.append((path, key, "|".join(watched)))
                if key in allow:
                    used.add(key)
                else:
                    problems.append("%s: %s row %r %s with origin ANY: give it an origin rule, or put it on ALLOW in "
                                    "tools/check_console_rules.py with the reason" % (path, name, key[1],
                                                                                     " and ".join(watched)))
    for key in sorted(allow - used):
        problems.append("ALLOW entry %s %r matches no transmitting or restarting row: remove it" % key)
    return problems, open_rows


# ---- the registration tie ---------------------------------------------------------------------------------------

def literal(arg):
    m = re.fullmatch(r'"([^"]*)"', arg.strip())
    return m.group(1) if m else None


def mesh_commands(sources):
    """[(name, table or None)] of MESH_CONSOLE_CMDS, the table mesh_cmd.cpp's loop registers."""
    for _path, text in sources:
        clean = strip_comments(text)
        m = re.search(r"\bmesh_con_t\s+const\s+MESH_CONSOLE_CMDS\s*\[[^\]]*\]\s*=\s*\{", clean)
        if m:
            body = clean[m.end() - 1:closing(clean, m.end() - 1, "{", "}") + 1]
            out = []
            for item in split_top(body[1:-1]):
                fields = split_top(re.sub(r"^\[\s*\w+\s*\]\s*=\s*", "", item)[1:-1])
                table = fields[-1] if len(fields) == 3 else "?"
                out.append((literal(fields[0]), None if table == "NULL" else table))
            return out
    return []


def app_more_tables(sources):
    """The tables lichen_sys_app_console_more hands to "fw app" (lichen_api registers it with s_more_rules)."""
    found = set()
    for _path, text in sources:
        clean = strip_comments(text)
        for m in re.finditer(r"(?<![\w])lichen_sys_app_console_more\s*\(", clean):
            if re.search(r"\bvoid\s+$", clean[:m.start()].rsplit("\n", 1)[-1]):
                continue
            args = split_top(clean[m.end():closing(clean, m.end() - 1, "(", ")")])
            found.add(args[-1])
    return found


def registrations(sources):
    """(with a table: {(name, table)}, without: {name}, problems) of every console_register* call in the firmware."""
    sources = list(sources)
    pairs, plain, problems = set(), set(), []
    mesh = mesh_commands(sources)
    more = app_more_tables(sources)
    for path, text in sources:
        clean = strip_comments(text)
        for m in CALL.finditer(clean):
            if re.search(r"\bvoid\s+$", clean[:m.start()].rsplit("\n", 1)[-1]):
                continue  # a declaration or the definition
            args = split_top(clean[m.end():closing(clean, m.end() - 1, "(", ")")])
            app, rules = m.group(1) == "_app", m.group(2) == "_rules"
            name_arg = args[1] if app else args[0]
            if "MESH_CONSOLE_CMDS[" in name_arg:
                if not mesh:
                    problems.append("%s: MeshCore's register loop, but no MESH_CONSOLE_CMDS table found" % path)
                for name, table in mesh:
                    (pairs.add((name, table)) if table else plain.add(name))
                continue
            name = literal(name_arg)
            if name is None:
                problems.append("%s: a console_register call whose name the check cannot read: %s" % (path, name_arg))
                continue
            if not rules:
                plain.add(name)
                continue
            table = args[-1].strip()
            if table == "s_more_rules":
                if not more:
                    problems.append("%s: \"%s\" registers lichen_sys_app_console_more's table, set nowhere" % (path,
                                                                                                             name))
                for t in more:
                    (pairs.add((name, t)) if t != "NULL" else plain.add(name))
            elif table == "NULL":
                plain.add(name)
            elif re.fullmatch(r"[A-Z][A-Z0-9_]*", table):
                pairs.add((name, table))
            else:
                problems.append("%s: \"%s\" registers a table the check cannot resolve: %s" % (path, name, table))
    return pairs, plain, problems


def pin_pairs(text):
    """{(name, table)} of the pin's RULES[] (host_tests/console_pin_test.c)."""
    clean = strip_comments(text)
    m = re.search(r"\brules_of_t\s+const\s+RULES\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        raise ValueError("no RULES[] table in the pin")
    body = clean[m.end() - 1:closing(clean, m.end() - 1, "{", "}") + 1]
    out = set()
    for item in split_top(body[1:-1]):
        fields = split_top(item.strip()[1:-1])
        if len(fields) != 2 or literal(fields[0]) is None:
            raise ValueError("a pin RULES[] entry the check cannot read: %s" % item)
        out.add((literal(fields[0]), fields[1].strip()))
    return out


def check_tie(sources, pin_text):
    """Problems where the firmware's registered tables and the pin's RULES[] differ."""
    pairs, plain, problems = registrations(sources)
    try:
        pinned = pin_pairs(pin_text)
    except ValueError as e:
        return problems + [str(e)]
    for name in sorted({n for n, _ in pairs} & plain):
        problems.append("\"%s\" is registered both with a sub-command table and without one" % name)
    for name, table in sorted(pinned - pairs):
        how = "without a table" if name in plain else "with %s" % (
            ", ".join(sorted(t for n, t in pairs if n == name)) or "nothing (not registered)")
        problems.append("the pin tests \"%s\" with %s, but the firmware registers it %s" % (name, table, how))
    for name, table in sorted(pairs - pinned):
        problems.append("the firmware registers \"%s\" with %s, which the pin's RULES[] does not test" % (name, table))
    return problems


def sources():
    for sub in ("components", "main"):
        for p in sorted((FW / sub).rglob("*")):
            if p.suffix in (".c", ".cpp", ".h") and "build" not in p.relative_to(FW).parts:
                if "managed_components" in p.parts:
                    continue
                yield p.relative_to(ROOT), p.read_text(errors="replace")


def selftest():
    src = """
#define TX CON_F_TX
#define OPEN CON_GATE_OPEN, CON_ORIGIN_ANY
static console_sub_t const A_RULES[] = {
    {"open", CON_MATCH_EXACT, OPEN, TX},
    {"usb", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_TX},
    {CON_DEV_WORD("dev"), CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_RESTART},
    {NULL, CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_TX},
    CONSOLE_SUB_END,
};
"""
    problems, open_rows = check_rows([("t.c", src)], {("A_RULES", "open")})
    assert sorted(k for _, k, _ in open_rows) == [("A_RULES", "dev"), ("A_RULES", "open")], open_rows
    assert len(problems) == 1 and "'dev'" in problems[0], problems
    print("check_console_rules selftest ok (the fixtures: tools/tests/check_console_rules_test.py)")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    srcs = list(sources())
    problems, open_rows = check_rows(srcs, ALLOW)
    problems += check_tie(srcs, PIN.read_text())
    if "--list" in sys.argv:
        for path, (name, word), what in open_rows:
            print("%-14s %-18s %-16s %s" % (what, name, word, path))
    for p in problems:
        print("check_console_rules: " + p)
    print("console rules: %d transmitting or restarting rows open to any origin, %d on the allow list; registered "
          "tables match the pin" % (len(open_rows), len(ALLOW)) if not problems else "console rules: FAILED")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
