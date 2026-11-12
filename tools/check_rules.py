#!/usr/bin/env python3
"""Source rule checks that were held only by prose and review (#287, from the #280 audit). Run by tools/static_checks.sh.

Checks (python3 -I tools/check_rules.py [--root DIR] [--baseline-dir DIR] [check ...]; default: all):
  cs-psram   #271  a portENTER/EXIT_CRITICAL (or task...) section touches PSRAM or calls an allocator / LVGL.
  log-level  #113  an ESP_LOGE line whose text says retry / again / ignored / dropped / skipped: that is a W by policy.
  task-class #269  a task started with LICHEN_STACK_LP_RAM or LICHEN_STACK_PSRAM must never return (no `return`, no
                   `break` out of its loop, last statement an endless loop or a noreturn call); a PSRAM one must also
                   not call NVS / flash / file APIs, directly or through a function of the same file.
  lvgl-task  #118  an lv_* call in a task entry function (the function given to xTaskCreate* / lichen_task_start*)
                   outside a lichen_sys_ui_lock ... lichen_sys_ui_unlock (or lvgl_port_lock ... unlock) span.
  task-delete #358 a function given to lichen_task_start / lichen_task_start_in (any stack), or a same-file function it
                   calls, deletes its own task (vTaskDelete(NULL) or of xTaskGetCurrentTaskHandle()): it must return.

It is a lexer plus brace matching, not a compiler, so each check states its limits next to its code. Findings are
ratcheted by tools/rules-baseline-<check>.txt (lines "key<TAB>count"): a finding not in the file, or above its count,
fails; the file may only shrink (growing it needs Lucas). log-level has a reviewed allow-list instead
(tools/log-level-allow.txt: "file<TAB>text fragment<TAB>why"). The baselines are empty or absent when the tree is clean.
"""
import argparse
import os
import re
import sys

SRC_DIRS = ("firmware/main", "firmware/components", "shared", "c6/main")
SKIP_PARTS = ("managed_components", "thirdparty", "/build/")
EXTS = (".c", ".cpp", ".h")
KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "do", "else"}
ENDLESS_RE = re.compile(r"^(for\s*\([^;()]*;\s*;[^;()]*\)|while\s*\(\s*(1|true)\s*\))\s*$")
NORETURN = {"esp_deep_sleep_start", "esp_restart", "abort", "esp_system_abort",
           "lichen_sys_power_off_now"}  # the last one is declared noreturn in lichen_sys_power.h


# ---- lexer ------------------------------------------------------------------------------------------------------

def lex(src):
    """Returns (code, text): both as long as src, newlines kept. code has comments and string/char literals blanked
    (their quotes stay); text has only the comments blanked, so a log message can be read from it."""
    code, text = list(src), list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            while src[j - 1] == "\\" and j < n:  # line continuation keeps the comment going
                k = src.find("\n", j + 1)
                j = n if k < 0 else k
            for k in range(i, j):
                code[k] = text[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if src[k] != "\n":
                    code[k] = text[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if src[k] != "\n":
                    code[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(code), "".join(text)


def scan_brackets(text, open_i):
    """Walks the call whose "(" is at open_i, skipping string and char literals. Returns (top-level arguments, index of
    the closing ")"), or ([], -1)."""
    args, depth, cur, i, n = [], 0, [], open_i, len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if ch in "([{":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif ch in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return args, i
        if ch == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
        i += 1
    return [], -1


def split_args(text, open_i):
    return scan_brackets(text, open_i)


class Func:
    def __init__(self, name, start, end):
        self.name, self.start, self.end = name, start, end  # start = "{", end = its "}"


def find_functions(code):
    """Every function body: a "{" right after a ")" (and const/noexcept/override) whose "(" follows an identifier that is
    not a control keyword, outside any other function. Methods in classes and namespaces count; lambdas and
    constructor initialiser lists are not understood (an initialiser's "x(1) {" is taken as a function called x)."""
    funcs, stack = [], []  # stack entries: Func or None
    for i, ch in enumerate(code):
        if ch == "{":
            f = None
            if not any(stack):
                m = re.search(r"\)\s*(?:const\s*|noexcept\s*|override\s*)*$", code[max(0, i - 200):i])
                if m:
                    close = max(0, i - 200) + m.start()
                    depth, j = 0, close
                    while j >= 0:
                        depth += 1 if code[j] == ")" else -1 if code[j] == "(" else 0
                        if depth == 0:
                            break
                        j -= 1
                    nm = re.search(r"([A-Za-z_]\w*)\s*$", code[:j])
                    if nm and nm.group(1) not in KEYWORDS:
                        f = Func(nm.group(1), i, -1)
            stack.append(f)
        elif ch == "}" and stack:
            f = stack.pop()
            if f:
                f.end = i
                funcs.append(f)
    return funcs


def func_at(funcs, pos):
    for f in funcs:
        if f.start < pos < f.end:
            return f.name
    return "-"


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


def tree_files(root):
    for d in SRC_DIRS:
        for dp, _dn, fns in os.walk(os.path.join(root, d)):
            if any(p in dp + "/" for p in SKIP_PARTS):
                continue
            for fn in sorted(fns):
                if fn.endswith(EXTS):
                    yield os.path.join(dp, fn)


def load(path):
    with open(path, errors="ignore") as f:
        src = f.read()
    code, text = lex(src)
    return src, code, text


# ---- cs-psram ---------------------------------------------------------------------------------------------------
# Limits: PSRAM-ness is guessed from names. A variable counts as PSRAM when its file declares it EXT_RAM_BSS_ATTR or
# assigns it (also as a `->field`) from lichen_alloc(.. LICHEN_MEM_BULK ..), heap_caps_*(.. SPIRAM ..) or lv_malloc;
# a section that reaches PSRAM through a pointer from another file, a function call or an array of structs is missed
# (false negative). A section is also flagged for any allocator or LVGL call (they take locks and may be PSRAM), a
# conservative call that can be a false positive; fix the code or baseline it after review. Only portENTER/EXIT_CRITICAL*
# and taskENTER/EXIT_CRITICAL* sections are seen: portSET_INTERRUPT_MASK_FROM_ISR, portDISABLE_INTERRUPTS and
# vPortEnterCritical are not (none is used in our code).

CS_OPEN = re.compile(r"\b(?:port|task)ENTER_CRITICAL\w*\s*\(")
CS_CLOSE = re.compile(r"\b(?:port|task)EXIT_CRITICAL\w*\s*\(")
CS_CALLS = re.compile(r"\b(lichen_alloc|lichen_free|lichen_realloc|heap_caps_\w+|malloc|calloc|realloc|free|"
                      r"lv_malloc|lv_free|lv_\w+|lvgl_port_\w+|ESP_LOG\w)\s*\(")
PSRAM_DECL = re.compile(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:=[^;]*)?;")


def psram_names(code, funcs):
    """Returns (global names, {function: local names}). EXT_RAM_BSS_ATTR objects and `->field` targets count for the
    whole file; a local pointer only inside the function that assigns it, so a `m` in one function does not taint another."""
    names, local = set(), {}
    for m in re.finditer(r"EXT_RAM_BSS_ATTR", code):
        decl = code[m.end():code.find(";", m.end()) + 1]
        d = re.match(r"\s*[\w\s\*]*?([A-Za-z_]\w*)\s*(?:\[|=|;)", decl) or PSRAM_DECL.search(decl)
        if d:
            names.add(d.group(1))
    for m in re.finditer(r"(->\s*)?([A-Za-z_]\w*)\s*=\s*(?:\([^)]*\)\s*)?(lichen_alloc|heap_caps_\w+|lv_malloc)\s*\(",
                         code):
        args = code[m.end():code.find(";", m.end())]
        if m.group(3) == "lv_malloc" or "SPIRAM" in args or "LICHEN_MEM_BULK" in args:
            fn = func_at(funcs, m.start())
            if m.group(1) or fn == "-":
                names.add(m.group(2))
            else:
                local.setdefault(fn, set()).add(m.group(2))
    return names, local


def section_end(code, start):
    """Index of the EXIT that closes the section opened just before start: the first EXIT at the same brace depth, so an
    early `EXIT; return;` inside a branch does not end it; a "}" that closes the enclosing block ends it too."""
    depth = 0
    for i in range(start, len(code)):
        ch = code[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                return i
        elif depth == 0 and code.startswith(("portEXIT_CRITICAL", "taskEXIT_CRITICAL"), i):
            return i
    return len(code)


def check_cs_psram(files):
    found = {}
    for path in files:
        _src, code, _text = load(path)
        if "CRITICAL" not in code:
            continue
        funcs = find_functions(code)
        gnames, lnames = psram_names(code, funcs)
        pos = 0
        while True:
            m = CS_OPEN.search(code, pos)
            if not m:
                break
            end = section_end(code, m.end())
            pos = end
            if "#define" in code[code.rfind("\n", 0, m.start()):m.start() + 20]:
                continue
            body = code[m.end():end]
            fn = func_at(funcs, m.start())
            for cm in CS_CALLS.finditer(body):
                found.setdefault((path, fn, cm.group(1)), []).append(line_of(code, m.end() + cm.start()))
            for nm in gnames | lnames.get(fn, set()):
                for _ in re.finditer(r"(?<![\w.])" + re.escape(nm) + r"\b|->\s*" + re.escape(nm) + r"\b", body):
                    found.setdefault((path, fn, "psram:" + nm), []).append(line_of(code, m.start()))
    return found


# ---- log-level --------------------------------------------------------------------------------------------------
# Limits: the message is read from the format string literal(s) only, so a word that arrives through a %s argument is
# not seen (--report lists those lines); a message that merely mentions the word of something that failed for good is a false positive: reword it
# or put it on tools/log-level-allow.txt with the reason.

LOG_WORDS = re.compile(r"\b(retry|retries|retrying|retried|again|ignored|ignoring|dropped|dropping|skipped|skipping)\b",
                       re.I)


LOGE_CALL = re.compile(r"\bESP_(?:EARLY_|DRAM_)?LOGE\s*\(|\b(ESP_LOG_LEVEL)\s*\(\s*ESP_LOG_ERROR\s*,")
NONLITERAL = []  # "file:line" of an error log whose format is not a literal: its words are not read (see --report)


def check_log_level(files, allow):
    found = {}
    for path in files:
        src, code, text = load(path)
        for m in LOGE_CALL.finditer(code):
            args, end = split_args(text, text.index("(", m.start()))
            fmt_i = 2 if m.group(1) else 1  # ESP_LOG_LEVEL(level, tag, fmt, ...) / ESP_LOGE(tag, fmt, ...)
            if end < 0 or len(args) <= fmt_i:
                continue
            lits = " ".join(re.findall(r'"((?:[^"\\]|\\.)*)"', args[fmt_i]))
            if not lits:
                NONLITERAL.append("%s:%d" % (os.path.relpath(path), line_of(src, m.start())))
            w = LOG_WORDS.search(lits)
            if w and not any(path.endswith(a[0]) and a[1] in lits for a in allow):
                found.setdefault((path, func_at(find_functions(code), m.start()), w.group(1).lower()), []).append(
                    line_of(src, m.start()))
    return found


# ---- task-class -------------------------------------------------------------------------------------------------
# Limits: only same-file callees are followed for the flash rule, and only by name; a call through a function pointer
# or into another file is not seen. "Never returns" is checked as: no `return`, no `break` that leaves the endless
# loop, and the last top-level statement an endless loop or a noreturn call. A task that exits through a callee that
# never returns, or a loop with a hidden exit (goto, a flag the condition reads) is outside what this sees. The flash
# rule applies to PSRAM stacks only (an LP RAM stack may write NVS, lichen_task.h).

FLASH_CALLS = re.compile(r"\b(nvs_\w+|esp_flash_\w+|spi_flash_\w+|esp_partition_\w+|esp_ota_\w+|fopen|fread|fwrite|"
                         r"fclose|fsync|opendir|readdir|mkdir|unlink|rename|stat|f_\w+|sdmmc_\w+|esp_vfs_\w+|"
                         r"lichen_fs_\w+|lichen_settings_\w+|lichen_store_\w+)\s*\(")
CALL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
TASK_START = re.compile(r"\blichen_task_start_in\s*\(")
BLOCK_KIND = re.compile(r"(\bfor|\bwhile|\bswitch)\s*\(")


def brace_kinds(code, lo, hi):
    """Walks code[lo:hi] (a function body without its braces). Yields (index, 'break'/'return'/'loop-open'...) events
    needed to decide whether the body can end: returns (has_return, leaks_break, last_stmt_endless, last_noreturn)."""
    has_return = bool(re.search(r"\breturn\b", code[lo:hi]))
    # top-level statements: split at depth 0 on ";" and on the "}" that closes a block
    depth, stmt_start, stmts = 0, lo, []
    for i in range(lo, hi):
        ch = code[i]
        if ch in "{(":
            depth += 1
        elif ch in "})":
            depth -= 1
            if ch == "}" and depth == 0:
                stmts.append((stmt_start, i + 1))
                stmt_start = i + 1
        elif ch == ";" and depth == 0:
            stmts.append((stmt_start, i + 1))
            stmt_start = i + 1
    stmts = [s for s in stmts if code[s[0]:s[1]].strip()]
    last = stmts[-1] if stmts else None
    endless, leaks, noret = False, False, False
    if last:
        s = code[last[0]:last[1]]
        head = s[:s.find("{")] if "{" in s else s
        endless = bool(ENDLESS_RE.match(head.strip())) and "{" in s
        noret = any(c in NORETURN for c in CALL_RE.findall(s)) and "{" not in s
        if endless:
            leaks = loop_leaks(code, last[0] + s.find("{") + 1, last[1] - 1)
    return has_return, leaks, endless, noret


def loop_leaks(code, lo, hi):
    """True when a `break` in code[lo:hi] (the endless loop's body) leaves that loop: not inside a nested loop or
    switch. A brace-less nested loop or switch is not understood."""
    stack = []
    for i in range(lo, hi):
        ch = code[i]
        if ch == "{":
            before = code[max(lo, i - 200):i].rstrip()
            kind = "plain"
            if before.endswith(")"):
                j, depth = len(before) - 1, 0
                while j >= 0:
                    depth += 1 if before[j] == ")" else -1 if before[j] == "(" else 0
                    if depth == 0:
                        break
                    j -= 1
                if re.search(r"\b(for|while|switch)\s*$", before[:j]):
                    kind = "inner"
            elif re.search(r"\bdo$", before):
                kind = "inner"
            stack.append(kind)
        elif ch == "}" and stack:
            stack.pop()
        elif code.startswith("break", i) and not (i and (code[i - 1].isalnum() or code[i - 1] == "_")) \
                and not code[i + 5:i + 6].isalnum() and "inner" not in stack:
            return True
    return False


def task_functions(code, funcs, fn_name):
    return [f for f in funcs if f.name == fn_name]


def callee_closure(code, funcs, entry):
    byname = {f.name: f for f in funcs}
    seen, todo = set(), [entry.name]
    while todo:
        n = todo.pop()
        if n in seen or n not in byname:
            continue
        seen.add(n)
        body = code[byname[n].start:byname[n].end]
        todo.extend(c for c in CALL_RE.findall(body) if c in byname)
    return [byname[n] for n in seen]


def check_task_class(files):
    found = {}
    for path in files:
        _src, code, _text = load(path)
        if "lichen_task_start_in" not in code:
            continue
        funcs = find_functions(code)
        for m in TASK_START.finditer(code):
            args, _end = split_args(code, m.end() - 1)
            if len(args) < 7 or func_at(funcs, m.start()) == "lichen_task_start":
                continue  # the declaration, or the definition's own forwarding
            fn_name, mem = args[2], args[6]
            if "(" in fn_name:
                continue  # the declaration or definition of lichen_task_start_in itself
            if "LICHEN_STACK_INTERNAL" in mem or "TASK_STACK_INTERNAL" in mem:
                continue
            if not re.search(r"(LICHEN|TASK)_STACK_(LP_RAM|PSRAM)\b", mem):
                found.setdefault((path, fn_name, "class-not-a-literal"), []).append(line_of(code, m.start()))
                continue
            fns = task_functions(code, funcs, fn_name)
            if not fns:
                found.setdefault((path, fn_name, "task-function-not-in-file"), []).append(line_of(code, m.start()))
                continue
            f = fns[0]
            has_ret, leaks, endless, noret = brace_kinds(code, f.start + 1, f.end)
            if has_ret:
                found.setdefault((path, fn_name, "returns"), []).append(line_of(code, f.start))
            elif leaks:
                found.setdefault((path, fn_name, "breaks-out-of-loop"), []).append(line_of(code, f.start))
            elif not (endless or noret):
                found.setdefault((path, fn_name, "falls-off-the-end"), []).append(line_of(code, f.start))
            if re.search(r"\bvTaskDelete\s*\(", code[f.start:f.end]):
                # lichen_task.h: fn ends by returning; a self-delete leaves the book entry and, on these stacks, aborts
                found.setdefault((path, fn_name, "self-delete"), []).append(line_of(code, f.start))
            if "PSRAM" in mem:
                for g in callee_closure(code, funcs, f):
                    for c in FLASH_CALLS.finditer(code[g.start:g.end]):
                        found.setdefault((path, fn_name, "flash:" + c.group(1)), []).append(line_of(code, g.start))
    return found


# ---- lvgl-task --------------------------------------------------------------------------------------------------
# Limits: an entry function is found by name in the same file, after stripping casts and a leading "&"; one that is
# not an identifier, or not defined in the file (a wrapper's parameter, a function from another file), is a finding of
# its own (entry-not-in-file) rather than a silent skip, so a wrapper like stress_workers.c's is baselined with a
# reason. The entry's same-file callees are followed by name (a call made inside a lock span is not followed); calls
# through function pointers, other files and C++ virtuals are not. A lichen_ui_call callback is not an entry, so its
# lv_* are right. The lock span is a straight text order between lichen_sys_ui_lock / lvgl_port_lock and the next
# unlock, so an early unlock inside a branch ends it for the rest of the body (false positive, rare).

TASK_CREATE = re.compile(r"\b(xTaskCreate\w*|lichen_task_start\w*)\s*\(")
LV_CALL = re.compile(r"\b(lv_[a-z0-9_]+)\s*\(")
LOCK_RE = re.compile(r"\b(lichen_sys_ui_lock|lvgl_port_lock)\s*\(|\b(lichen_sys_ui_unlock|lvgl_port_unlock)\s*\(")


def entry_names(code, funcs):
    out = []
    for m in TASK_CREATE.finditer(code):
        if func_at(funcs, m.start()) in ("lichen_task_start", "lichen_task_start_in"):
            continue
        args, _end = split_args(code, m.end() - 1)
        idx = 0 if m.group(1).startswith("xTaskCreate") else 2
        if len(args) > idx and not re.search(r"\(\s*\*", args[idx]):  # not the starter's own declaration
            out.append(re.sub(r"^(?:\([^()]*\)\s*)*&?\s*", "", args[idx]).strip())
    return out


def lv_in_task(code, funcs, entry):
    """lv_* calls reachable from entry outside a lock span: [(function, call, position)]."""
    byname = {f.name: f for f in funcs}
    hits, seen, todo = [], set(), [entry]
    while todo:
        name = todo.pop()
        if name in seen or name not in byname:
            continue
        seen.add(name)
        f = byname[name]
        body = code[f.start:f.end]
        locked = 0
        events = [(m.start(), "lock" if m.group(1) else "unlock") for m in LOCK_RE.finditer(body)]
        events += [(m.start(), "lv:" + m.group(1)) for m in LV_CALL.finditer(body)]
        events += [(m.start(), "call:" + m.group(1)) for m in CALL_RE.finditer(body)
                   if m.group(1) in byname and m.group(1) != name]
        for p, ev in sorted(events):
            if ev == "lock":
                locked += 1
            elif ev == "unlock":
                locked = max(0, locked - 1)
            elif not locked and ev.startswith("lv:"):
                hits.append((entry, ev[3:], f.start + p))
            elif not locked and ev.startswith("call:"):
                todo.append(ev[5:])
    return hits


def check_lvgl_task(files):
    found = {}
    for path in files:
        _src, code, _text = load(path)
        if not TASK_CREATE.search(code):
            continue
        funcs = find_functions(code)
        for name in sorted(set(entry_names(code, funcs))):
            if not any(f.name == name for f in funcs):
                found.setdefault((path, name, "entry-not-in-file"), []).append(1)
                continue
            for entry, call, pos in lv_in_task(code, funcs, name):
                found.setdefault((path, entry, call), []).append(line_of(code, pos))
    return found


# ---- task-delete ------------------------------------------------------------------------------------------------
# Why: lichen_task.c's trampoline runs the function, then takes the task out of the book and deletes it (#181). A task
# that deletes itself never comes back, so its book slot stays taken until reboot (#358: one of 32 slots per boot).
# Limits: entries are found as in lvgl-task (same file, by name, casts and "&" stripped); one not defined in the file
# (a wrapper's parameter) is skipped here, lvgl-task already reports it as entry-not-in-file. Same-file callees are
# followed by name. A self-delete is vTaskDelete or vTaskDeleteWithCaps of NULL / 0 / nullptr /
# xTaskGetCurrentTaskHandle() (a (TaskHandle_t) cast allowed), or of a local the same function set from
# xTaskGetCurrentTaskHandle(); deleting another task by its handle is fine. Raw xTaskCreate* tasks are not looked at:
# they have no trampoline and must delete themselves. Not seen (false negatives): a self-delete behind a macro, a
# handle passed in or stored in a global / struct, entries or callees reached through function pointers or tables,
# and entries or callees defined in another file.

CAST = r"(?:\(\s*TaskHandle_t\s*\)\s*)?"
DELETE = r"\bvTaskDelete(?:WithCaps)?\s*\(\s*" + CAST
SELF_DELETE = re.compile(DELETE + r"(?:NULL|0|nullptr|xTaskGetCurrentTaskHandle\s*\(\s*\))\s*\)")
SELF_HANDLE = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*" + CAST + r"xTaskGetCurrentTaskHandle\s*\(\s*\)")


def self_deletes(body):
    """Positions in body of a self-delete: a literal one, or of a local set from xTaskGetCurrentTaskHandle()."""
    hits = [m.start() for m in SELF_DELETE.finditer(body)]
    for name in set(SELF_HANDLE.findall(body)):
        hits += [m.start() for m in re.finditer(DELETE + re.escape(name) + r"\s*\)", body)]
    return sorted(hits)
LICHEN_START = re.compile(r"\blichen_task_start(?:_in)?\s*\(")


def lichen_entries(code, funcs):
    out = []
    for m in LICHEN_START.finditer(code):
        if func_at(funcs, m.start()) in ("lichen_task_start", "lichen_task_start_in"):
            continue  # the definition's own forwarding
        args, _end = split_args(code, m.end() - 1)
        if len(args) > 2 and not re.search(r"\(\s*\*", args[2]):  # not the starter's own declaration
            out.append(re.sub(r"^(?:\([^()]*\)\s*)*&?\s*", "", args[2]).strip())
    return out


def check_task_delete(files):
    found = {}
    for path in files:
        _src, code, _text = load(path)
        if not LICHEN_START.search(code):
            continue
        funcs = find_functions(code)
        for name in sorted(set(lichen_entries(code, funcs))):
            fns = task_functions(code, funcs, name)
            if not fns:
                continue
            for g in callee_closure(code, funcs, fns[0]):
                for pos in self_deletes(code[g.start:g.end]):
                    found.setdefault((path, name, "self-delete-in-" + g.name), []).append(line_of(code, g.start + pos))
    return found


# ---- baseline and driver ----------------------------------------------------------------------------------------

def function_counts(files):
    """{extension: (files, functions found, files with a body but no function found)}: a parse that loses functions
    shows up as a count that drops or as files in the last column (--report)."""
    out = {}
    for path in files:
        _src, code, _text = load(path)
        n = len(find_functions(code))
        ext = os.path.splitext(path)[1]
        f, fn, empty = out.get(ext, (0, 0, 0))
        out[ext] = (f + 1, fn + n, empty + (1 if n == 0 and "{" in code else 0))
    return out


def key_str(root, k):
    return ":".join([os.path.relpath(k[0], root)] + list(k[1:]))


def read_baseline(path):
    base = {}
    if os.path.exists(path):
        for ln in open(path):
            if ln.strip() and not ln.startswith("#"):
                key, _, cnt = ln.rstrip("\n").rpartition("\t")
                base[key] = int(cnt)
    return base


def read_allow(path):
    out = []
    if os.path.exists(path):
        for ln in open(path):
            parts = ln.rstrip("\n").split("\t")
            if ln.strip() and not ln.startswith("#") and len(parts) >= 3:
                out.append((parts[0], parts[1]))
    return out


HELP = {
    "cs-psram": "keep PSRAM out of the critical section (copy first, or use a mutex), #271",
    "log-level": "ESP_LOGW for a retry / drop / skip (policy #113); reword it or add a reviewed line to tools/log-level-allow.txt",
    "task-class": "a task on an LP RAM / PSRAM stack never returns and (PSRAM) never touches flash (#269)",
    "lvgl-task": "no lv_* in a task: lichen_ui_call (apps) or lichen_sys_ui_lock ... unlock (OS), #118",
    "task-delete": "a lichen_task_start* task returns; its trampoline deletes it and frees the book slot (#181, #358)",
}


def run(root, base_dir, checks, quiet=False):
    files = list(tree_files(root))
    allow = read_allow(os.path.join(base_dir, "log-level-allow.txt"))
    results = {
        "cs-psram": lambda: check_cs_psram(files),
        "log-level": lambda: check_log_level(files, allow),
        "task-class": lambda: check_task_class(files),
        "lvgl-task": lambda: check_lvgl_task(files),
        "task-delete": lambda: check_task_delete(files),
    }
    bad = 0
    for name in checks:
        found = results[name]()
        base = read_baseline(os.path.join(base_dir, "rules-baseline-%s.txt" % name))
        counts = {key_str(root, k): len(v) for k, v in found.items()}
        lines = {key_str(root, k): v for k, v in found.items()}
        for key, cnt in sorted(counts.items()):
            if cnt > base.get(key, 0):
                bad += 1
                print("rule check %s: %s x%d (baseline %d), line %s: %s" % (
                    name, key, cnt, base.get(key, 0), lines[key][0], HELP[name]), file=sys.stderr)
        for key, cnt in sorted(base.items()):
            if counts.get(key, 0) < cnt and not quiet:
                print("note: rule check %s: %s dropped to %d, shrink tools/rules-baseline-%s.txt" % (
                    name, key, counts.get(key, 0), name), file=sys.stderr)
    return bad


def main(argv):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.path.dirname(here))
    ap.add_argument("--baseline-dir", default=here)
    ap.add_argument("--report", action="store_true", help="print every finding with its lines, ignore baselines")
    ap.add_argument("checks", nargs="*", default=list(HELP))
    a = ap.parse_args(argv)
    if a.report:
        files = list(tree_files(a.root))
        fns = {"cs-psram": check_cs_psram, "task-class": check_task_class, "lvgl-task": check_lvgl_task,
               "task-delete": check_task_delete, "log-level": lambda f: check_log_level(f, [])}
        for name in a.checks:
            for k, v in sorted(fns[name](files).items()):
                print("%s\t%s\t%d\t%s" % (name, key_str(a.root, k), len(v), ",".join(map(str, v[:5]))))
        for ext, (nf, nfn, empty) in sorted(function_counts(files).items()):
            print("parsed %s: %d files, %d functions, %d files with braces but no function" % (ext, nf, nfn, empty))
        for loc in NONLITERAL:
            print("log-level: error log with a non-literal format (words not read): %s" % loc)
        return 0
    bad = run(a.root, a.baseline_dir, a.checks)
    if bad:
        print("rule checks: %d finding(s) over the baseline" % bad, file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
