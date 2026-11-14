#!/usr/bin/env python3
"""Console table guard (#243): count the console_register() and console_register_dev() calls this image reaches and
fail above 80 % of MAX_COMMANDS in firmware/components/fw_console/console.c. console_register_dev calls count only
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
CALL = re.compile(r"(?<![\w])console_register(_dev|_app)?\s*\(")
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


def count_calls(text, cfg):
    """Number of reachable console_register calls in one source text."""
    n = 0
    stack = []  # [active_now, parent_active, taken_before]
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
            continue
        if active:
            for c in CALL.finditer(line):
                if DECL.search(line[: c.start()]):
                    continue
                if c.group(1) == "_dev" and not cfg.get("CONFIG_LICHEN_DEV_COMMANDS", False):
                    continue
                n += 1
    return n


def sources():
    for sub in ("components", "main"):
        for p in sorted((FW / sub).rglob("*")):
            if p.suffix in (".c", ".cpp") and "build" not in p.relative_to(FW).parts:
                yield p


def max_commands():
    m = re.search(r"#define\s+MAX_COMMANDS\s+(\d+)", (FW / "components/fw_console/console.c").read_text())
    if not m:
        sys.exit("check_console_slots: MAX_COMMANDS not found in console.c")
    return int(m.group(1))


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
    total = sum(count_calls(p.read_text(errors="replace"), cfg) for p in sources())
    limit = max_commands()
    print(f"console commands: {total} reachable of {limit} slots (fail above {LIMIT_PERCENT} % = {limit * LIMIT_PERCENT // 100})")
    if total * 100 > limit * LIMIT_PERCENT:
        print("check_console_slots: raise MAX_COMMANDS in firmware/components/fw_console/console.c (12 B per slot)")
        return 1
    return 0


sys.exit(main())
