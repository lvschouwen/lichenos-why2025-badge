#!/usr/bin/env python3
"""Scaffold a new Lichen app (#385): `tools/badge.sh new-app <id> "<Name>"`, run with python3 -I. A host-only tool: it
only reads and writes files of the repo tree and never reaches a badge.

Creates firmware/components/lichen_<id>/ from tools/app_template/ (CMake, lichen_<id>.h, app_<id>.c, <id>_model.c/.h)
and host_tests/<id>_model_test.c, then registers the app in four shared files, each insert idempotent (a line that is
already there is not added again):
  firmware/components/lichen_apps/CMakeLists.txt  PRIV_REQUIRES gets lichen_<id>
  firmware/components/lichen_apps/lichen_apps.c   the include and the <id>_app_register() call (last, so it is the
                                                  last tile on home)
  tools/layers.conf                               component, app and app-objects lines (layer L6, rule D)
  host_tests/CMakeLists.txt                       the <id>_model_test target and its add_test
Everything is validated before the first file is touched, and the new files are written before the shared ones, so a
refusal leaves the tree as it was. --root points at another tree (the gate scaffolds a throwaway app into a temp copy
with it: tools/tests/new_app_test.sh)."""
import argparse
import errno
import os
import re
import sys
import tempfile

TEMPLATE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app_template")
ID_RE = re.compile(r"^[a-z][a-z0-9_]{0,12}$")  # lichen_app.h: 1..13 of a-z0-9_; a letter first, it becomes C identifiers
NAME_MAX = 24                                  # a tile and a menu title are narrow
BUILTIN_RESERVED = ("c6", "crash", "lichen", "test")  # lichen_register_app refuses these (lichen_app.h)


class Refused(Exception):
    pass


def check_id(app_id):
    if not ID_RE.match(app_id):
        raise Refused("id '%s': 1-13 characters of a-z, 0-9 and _, starting with a letter (it names C functions and "
                      "the app's settings namespace)" % app_id)


def check_name(name):
    """The name goes into generated C as a string literal and into comments: printable ASCII only, and not the
    characters that would end or escape a literal (a quote, a backslash), start a trigraph (?) or a template slot (@)."""
    if not name or len(name) > NAME_MAX:
        raise Refused("name: 1-%d characters" % NAME_MAX)
    for ch in name:
        if not (" " <= ch <= "~"):
            raise Refused("name: printable ASCII only (no newline, tab or other control character)")
        if ch in '"\\?@':
            raise Refused("name: no quote, backslash, question mark or @ (it goes into C source as a string)")
    if name != name.strip():
        raise Refused("name: no blank at either end")


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def reserved_ids(root):
    text = read(os.path.join(root, "firmware/components/lichen_api/fs_path.c"))
    m = re.search(r"RESERVED_IDS\[\]\s*=\s*\{([^}]*)\}", text)
    if not m:
        raise Refused("RESERVED_IDS not found in lichen_api/fs_path.c: is --root a Lichen tree?")
    return set(re.findall(r'"([^"]+)"', m.group(1))) | set(BUILTIN_RESERVED)


def source_files(root):
    for top in ("firmware/components", "firmware/main"):
        for dp, dns, fns in os.walk(os.path.join(root, top)):
            dns[:] = [d for d in dns if d not in ("build", "managed_components")]
            for fn in fns:
                if fn.endswith((".c", ".h", ".cpp", ".hpp")):
                    yield os.path.join(dp, fn)


def find_traces(root, app_id):
    """Everything that says the id is taken: a reserved id, a component, an app id or an app_register symbol of that
    name in any source, an `app` line in layers.conf."""
    found = []
    if app_id in reserved_ids(root):
        found.append("the id is reserved by the OS")
    if os.path.exists(os.path.join(root, "firmware/components/lichen_" + app_id)):
        found.append("firmware/components/lichen_%s exists" % app_id)
    pats = (re.compile(r'\.id\s*=\s*"%s"' % app_id), re.compile(r"\b%s_app_register\b" % app_id),
            re.compile(r"\b%s_model_" % app_id))
    for path in source_files(root):
        text = read(path)
        if any(p.search(text) for p in pats):
            found.append("%s already uses it" % os.path.relpath(path, root))
    test_src = os.path.join(root, "host_tests/%s_model_test.c" % app_id)
    if os.path.exists(test_src):
        found.append("host_tests/%s_model_test.c exists" % app_id)
    if re.search(r"(add_executable|add_test)\(\s*(NAME\s+)?%s_model_test\b" % app_id,
                 read(os.path.join(root, "host_tests/CMakeLists.txt"))):
        found.append("host_tests/CMakeLists.txt has a %s_model_test target" % app_id)
    if re.search(r"^app\s+%s\s" % app_id, read(os.path.join(root, "tools/layers.conf")), re.M):
        found.append("tools/layers.conf has an app line for it")
    return found


def render(text, subst):
    """One pass, so a value is never itself substituted again."""
    return re.sub(r"@(ID|UP|NAME_C)@", lambda m: subst[m.group(1)], text)


def template_files(app_id):
    """(destination relative to the repo root, template file) for every file of the new app."""
    comp = "firmware/components/lichen_%s/" % app_id
    return [(comp + "CMakeLists.txt", "CMakeLists.txt"), (comp + "lichen_%s.h" % app_id, "lichen_@ID@.h"),
            (comp + "app_%s.c" % app_id, "app_@ID@.c"), (comp + "%s_model.c" % app_id, "@ID@_model.c"),
            (comp + "%s_model.h" % app_id, "@ID@_model.h"), ("host_tests/%s_model_test.c" % app_id, "@ID@_model_test.c")]


# ---- the shared files: each returns the new text, or the same text when the line is already there ---------------------

def insert_after_last(text, lines, anchor_re, what):
    """The lines not yet in text go after the last line that matches anchor_re (together, in order)."""
    have = set(text.splitlines())
    new = [ln for ln in lines if ln not in have]
    if not new:
        return text
    last = None
    for last in re.finditer(anchor_re, text, re.M):
        pass
    if not last:
        raise Refused("%s: no place to insert (no line matches /%s/)" % (what, anchor_re))
    return text[:last.end()] + "\n" + "\n".join(new) + text[last.end():]


def edit_cmake(text, app_id):
    word = "lichen_" + app_id
    m = re.search(r"PRIV_REQUIRES([^)]*)\)", text)
    if not m:
        raise Refused("lichen_apps/CMakeLists.txt: no PRIV_REQUIRES list")
    if word in m.group(1).split():
        return text
    return text[:m.end(1)] + " " + word + text[m.end(1):]


def edit_apps_c(text, app_id):
    inc = '#include "lichen_%s.h"' % app_id
    call = "    %s_app_register();" % app_id
    if inc not in text.splitlines():
        first = re.search(r'^#include "lichen_apps.h"$', text, re.M)
        if not first:
            raise Refused('lichen_apps.c: no #include "lichen_apps.h" line to put the include after')
        text = text[:first.end()] + "\n" + inc + text[first.end():]
    if call not in text.splitlines():
        end = text.rfind("\n}")
        if end < 0 or "lichen_apps_register" not in text:
            raise Refused("lichen_apps.c: no end of lichen_apps_register() found")
        text = text[:end + 1] + call + "\n" + text[end + 1:]
    return text


def edit_layers(text, app_id):
    text = insert_after_last(text, ["component lichen_%s L6" % app_id], r"^component mesh lichen_apps L6$", "layers.conf")
    return insert_after_last(text, ["app %s lichen_%s - INTERNAL_RAM=1 PSRAM=0" % (app_id, app_id),
                                    "app-objects %s app_%s.c %s_model.c" % (app_id, app_id, app_id)],
                             r"^app-objects .*$", "layers.conf")


def edit_host_cmake(text, app_id):
    """Appends the target and its test, each only when it is not there yet."""
    d = {"id": app_id}
    parts = (("add_executable(%(id)s_model_test ", "add_executable(%(id)s_model_test ${CMAKE_CURRENT_SOURCE_DIR}/%(id)s_model_test.c "
              "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_%(id)s/%(id)s_model.c)\n"
              "target_include_directories(%(id)s_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_%(id)s)\n"
              "target_compile_options(%(id)s_model_test PRIVATE -Wall -Wextra -Werror)\n"),
             ("add_test(NAME %(id)s_model_test ", "add_test(NAME %(id)s_model_test COMMAND %(id)s_model_test)\n"))
    add = "".join(body % d for key, body in parts if (key % d) not in text)
    if not add:
        return text
    return text.rstrip("\n") + "\n\n# %s: the scaffolded app's rules (tools/badge.sh new-app, #385)\n" % app_id + add


SHARED = (("firmware/components/lichen_apps/CMakeLists.txt", edit_cmake),
          ("firmware/components/lichen_apps/lichen_apps.c", edit_apps_c),
          ("tools/layers.conf", edit_layers), ("host_tests/CMakeLists.txt", edit_host_cmake))


def write_atomic(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        # Test hook (tools/tests/new_app_test.sh): fail after the temp file exists, as a full disk would
        if os.environ.get("NEW_APP_TEST_FAIL_AFTER_TEMP") and path.endswith(os.environ["NEW_APP_TEST_FAIL_AFTER_TEMP"]):
            raise OSError(errno.ENOSPC, "No space left on device (test hook)")
        os.replace(tmp, path)
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def scaffold(root, app_id, name):
    check_id(app_id)
    check_name(name)
    traces = find_traces(root, app_id)
    if traces:
        raise Refused("id '%s' is taken: %s" % (app_id, "; ".join(traces)))
    subst = {"ID": app_id, "UP": app_id.upper(), "NAME_C": name}
    new_files = [(dest, render(read(os.path.join(TEMPLATE_DIR, src)), subst)) for dest, src in template_files(app_id)]
    edited = []
    for rel, fn in SHARED:  # every anchor is looked up before anything is written
        old = read(os.path.join(root, rel))
        edited.append((rel, fn(old, app_id), old))
    created = []
    done = []  # shared files already replaced: (path, old text)
    try:
        for dest, text in new_files:
            path = os.path.join(root, dest)
            created.append(path)
            write_atomic(path, text)
        for rel, new, old in edited:
            if new != old:
                path = os.path.join(root, rel)
                write_atomic(path, new)
                done.append((path, old))
    except OSError as e:
        left = rollback(root, app_id, created, done)
        if left:
            raise Refused("writing failed (%s) and the undo failed too: partial state is left, remove or restore by "
                          "hand: %s" % (e, ", ".join(left)))
        raise Refused("writing failed (%s); every file this run wrote was removed or restored, the tree is as it was" % e)
    return [d for d, _ in new_files] + [r for r, n, o in edited if n != o]


def rollback(root, app_id, created, done):
    """Undo a half-done run: restore the shared files already replaced, remove the new files and the new directory
    (the id was free, so the component directory is ours). A step that fails too is skipped, the rest still runs; the
    paths that could not be undone are returned (empty: the tree is as it was)."""
    left = []
    for path, old in done:
        try:
            write_atomic(path, old)
        except OSError:
            left.append(os.path.relpath(path, root))
    for path in created:
        try:
            os.remove(path)
        except FileNotFoundError:
            pass  # the write that failed never got as far as the file
        except OSError:
            left.append(os.path.relpath(path, root))
    try:
        os.rmdir(os.path.join(root, "firmware/components/lichen_" + app_id))
    except FileNotFoundError:
        pass
    except OSError:
        left.append("firmware/components/lichen_" + app_id)
    return left


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("id")
    ap.add_argument("name")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    try:
        changed = scaffold(root, args.id, args.name)
    except Refused as e:
        print("new-app: " + str(e), file=sys.stderr)
        return 1
    print("new-app: %s ('%s') created; %d files written or changed:" % (args.id, args.name, len(changed)))
    for rel in changed:
        print("  " + rel)
    print("next: build (./firmware/build.sh build); the tile 'on home' shows once the app has counted something.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
