#!/usr/bin/env python3
"""Console table guard (#243): count the console_register() and console_register_dev() calls this image reaches and
fail above 80 % of MAX_COMMANDS in firmware/components/fw_console/console.c; and the sub-command tables they register
(#464) against 80 % of MAX_RULE_TABLES (a command whose table finds no room is not registered at all). MeshCore's
loop over MESH_CONSOLE_CMDS counts every command of that table. console_register_dev calls count only
with CONFIG_LICHEN_DEV_COMMANDS set (#373): without it they are empty inlines and take no slot.

Why a source count and not a boot check: it fails at build time, on the machine that adds the 40th command, not on
a bench badge after flashing. #if/#ifdef blocks on CONFIG_* are evaluated against firmware/sdkconfig (a block whose
condition is not a plain CONFIG_ test is counted, so the check errs on the high side). Calls inside comments do not
count. Usage: check_console_slots.py [--selftest] [--sdkconfig PATH] (default firmware/sdkconfig)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"
LIMIT_PERCENT = 80
CALL = re.compile(r"(?<![\w])console_register(_dev|_app)?(_rules)?\s*\(")  # _rules: with a sub-command table (#464)
DECL = re.compile(r"(?:void|int)\s+$")


def load_config(path):
    cfg = {}
    for line in path.read_text().splitlines():
        m = re.match(r"(CONFIG_\w+)=(.*)", line)
        if m:
            cfg[m.group(1)] = m.group(2) not in ("", "n")
    return cfg


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def cond_value(expr, cfg):
    """True/False for a plain CONFIG_X, !CONFIG_X or defined(CONFIG_X) test, True (count it) for anything else."""
    e = expr.strip()
    m = re.fullmatch(r"(!?)\s*(?:defined\s*\(\s*)?(CONFIG_\w+)\s*\)?", e)
    if not m:
        return None
    val = cfg.get(m.group(2), False)
    return (not val) if m.group(1) else val


def active_lines(text, cfg):
    """The lines of text (comments stripped) that this sdkconfig compiles; the others are blanked, so lines keep
    their numbers."""
    out = []
    stack = []  # [parent_active, taken_before]
    active = True
    for line in strip_comments(text).splitlines():
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            kw, rest = m.groups()
            if kw in ("if", "ifdef", "ifndef"):
                v = cond_value(rest if kw == "if" else (("!" if kw == "ifndef" else "") + rest), cfg)
                cur = True if v is None else v
                stack.append([active, cur])
                active = active and cur
            elif kw == "elif":
                v = cond_value(rest, cfg)
                top = stack[-1]
                active = top[0] and not top[1] and (True if v is None else v)
                top[1] = top[1] or active
            elif kw == "else":
                top = stack[-1]
                active = top[0] and not top[1]
            elif stack:
                active = stack.pop()[0]
            out.append("")
            continue
        out.append(line if active else "")
    return out


def call_args(text, at):
    """The text of a call's arguments, from just after its "(" at text[at - 1], strings skipped."""
    depth, i = 1, at
    while i < len(text) and depth:
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and (text[i] != '"' or text[i - 1] == "\\"):
                i += 1
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return text[at:i - 1]


def reachable_calls(text, cfg, mesh):
    """(slots, rule tables) the reachable console_register calls of one source text take. mesh: (commands, tables)
    of MESH_CONSOLE_CMDS, which MeshCore's loop registers with one call (#464)."""
    lines = active_lines(text, cfg)
    joined = "\n".join(lines)
    offsets, pos = [], 0
    for line in lines:
        offsets.append(pos)
        pos += len(line) + 1
    slots = tables = 0
    for no, line in enumerate(lines):
        for c in CALL.finditer(line):
            if DECL.search(line[: c.start()]):
                continue
            if c.group(1) == "_dev" and not cfg.get("CONFIG_LICHEN_DEV_COMMANDS", False):
                continue
            args = call_args(joined, offsets[no] + c.end())
            if "MESH_CONSOLE_CMDS[" in args:
                slots += mesh[0]
                tables += mesh[1]
                continue
            slots += 1
            if c.group(2) and args.rsplit(",", 1)[-1].strip() != "NULL":
                tables += 1
    return slots, tables


def count_calls(text, cfg, mesh=(1, 0)):
    """Number of console table slots the reachable console_register calls of one source text take."""
    return reachable_calls(text, cfg, mesh)[0]


def mesh_commands():
    """(commands, of them with a rule table) in MeshCore's MESH_CONSOLE_CMDS (firmware/components/mesh)."""
    text = strip_comments((FW / "components/mesh/mesh_cmd_table.c").read_text())
    m = re.search(r"MESH_CONSOLE_CMDS\s*\[[^\]]*\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        sys.exit("check_console_slots: MESH_CONSOLE_CMDS not found in mesh_cmd_table.c")
    entries = re.findall(r"\[\s*MESH_CON_\w+\s*\]\s*=\s*\{(.*?)\}\s*,", m.group(1), re.S)
    with_table = sum(1 for e in entries if e.rsplit(",", 1)[-1].strip() != "NULL")
    return len(entries), with_table


def sources():
    for sub in ("components", "main"):
        for p in sorted((FW / sub).rglob("*")):
            if p.suffix in (".c", ".cpp") and "build" not in p.relative_to(FW).parts:
                yield p


def max_of(name):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, (FW / "components/fw_console/console.c").read_text())
    if not m:
        sys.exit("check_console_slots: %s not found in console.c" % name)
    return int(m.group(1))


def max_commands():
    return max_of("MAX_COMMANDS")


def selftest():
    cfg = {"CONFIG_ON": True}
    src = """
void console_register(char const *a);
console_register("a", "u", f);
// console_register("b", "u", f);
/* console_register("c", "u", f); */
#if CONFIG_ON
console_register("d", "u", f);
#else
console_register("e", "u", f);
#endif
#ifdef CONFIG_OFF
console_register("f", "u", f);
#elif CONFIG_ON
console_register("g", "u", f);
#endif
#if !CONFIG_ON
console_register("h", "u", f);
#endif
"""
    assert count_calls(src, cfg) == 3, count_calls(src, cfg)  # a, d, g
    dev = """
void console_register_dev(char const *a);
console_register_dev("i", "u", f);
console_register("j", "u", f);
"""
    assert count_calls(dev, {"CONFIG_LICHEN_DEV_COMMANDS": True}) == 2  # i, j
    assert count_calls(dev, {}) == 1  # j: a release build drops i
    app = 'console_register_app("q", "n", "u", f);\nconsole_register("j", "u", f);\n'
    assert count_calls(app, {}) == 2  # an app's command counts in a release build too (#423)
    rules = 'console_register_rules("s", "u", f, R);\nconsole_register_app_rules("q", "t", "u", f, R);\n'
    assert count_calls(rules, {}) == 2  # a command with a sub-command table takes a slot too (#464)
    assert reachable_calls(rules, {}, (1, 0)) == (2, 2)  # and a rule table each
    none = 'console_register_rules("s", "u (x)", f,\n                       NULL);\n'
    assert reachable_calls(none, {}, (1, 0)) == (1, 0)  # NULL: no table
    loop = 'console_register_app_rules("m", MESH_CONSOLE_CMDS[c.id].name, u, f,\n    MESH_CONSOLE_CMDS[c.id].rules);\n'
    assert reachable_calls(loop, {}, (7, 3)) == (7, 3)  # MeshCore's loop: every command of its table
    print("check_console_slots selftest ok")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    path = FW / "sdkconfig"
    if "--sdkconfig" in sys.argv:
        at = sys.argv.index("--sdkconfig") + 1
        if at >= len(sys.argv):
            sys.exit("check_console_slots: --sdkconfig needs a path")
        path = Path(sys.argv[at])
    cfg = load_config(path)
    mesh = mesh_commands()
    counts = [reachable_calls(p.read_text(errors="replace"), cfg, mesh) for p in sources()]
    failed = 0
    for what, total, name, cost in (("console commands", sum(c[0] for c in counts), "MAX_COMMANDS", "12 B per slot"),
                                    ("rule tables", sum(c[1] for c in counts), "MAX_RULE_TABLES", "4 B per table")):
        limit = max_of(name)
        print(f"{what}: {total} reachable of {limit} slots (fail above {LIMIT_PERCENT} % = "
              f"{limit * LIMIT_PERCENT // 100})")
        if total * 100 > limit * LIMIT_PERCENT:
            print(f"check_console_slots: raise {name} in firmware/components/fw_console/console.c ({cost}); an "
                  "overflow drops the command at boot")
            failed = 1
    return failed


sys.exit(main())
