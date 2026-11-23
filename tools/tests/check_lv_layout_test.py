#!/usr/bin/env python3
"""Host test (#522): tools/check_lv_layout.py passes when the firmware's and the app's flags give the same layout, fails
when a -D changes a size, and fails when the probe does not compile. A stand-in probe and the host compiler, no LVGL."""
import json
import os
import subprocess
import sys
import tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "check_lv_layout.py")
PROBE = "#ifndef SZ\n#define SZ 8\n#endif\nconst unsigned lv_layout[] = { sizeof(long), SZ, 3 };\n"
failed = []


def db(path, directory, src, flags):
    with open(path, "w") as f:
        json.dump([{"directory": directory, "file": src, "output": "x.o",
                    "command": "cc %s -MD -MT x.o -MF x.d -o x.o -c %s" % (flags, src)}], f)


def run(tmp, fw_flags, app_flags, probe=PROBE):
    os.makedirs(os.path.join(tmp, "main"), exist_ok=True)
    pr = os.path.join(tmp, "probe.c")
    with open(pr, "w") as f:
        f.write(probe)
    db(os.path.join(tmp, "fw.json"), tmp, os.path.join(tmp, "lichen_ui.c"), fw_flags)
    db(os.path.join(tmp, "app.json"), tmp, os.path.join(tmp, "main", "app.c"), app_flags)
    return subprocess.run([sys.executable, "-I", TOOL, "--fw", os.path.join(tmp, "fw.json"),
                           "--app", os.path.join(tmp, "app.json"), "--probe", pr], capture_output=True, text=True)


def expect(name, r, rc, text):
    if r.returncode != rc or text not in (r.stdout + r.stderr):
        failed.append("%s: exit %d, wanted %d with '%s': %s%s" % (name, r.returncode, rc, text, r.stdout, r.stderr))


with tempfile.TemporaryDirectory() as tmp:
    expect("same", run(tmp, "-DSZ=8", "-DSZ=8"), 0, "sizes and offsets match the firmware")
    expect("different size", run(tmp, "-DSZ=8", "-DSZ=16"), 1, "differ from the firmware's at probe entries")
    expect("broken probe", run(tmp, "-DSZ=8", "-DSZ=8", "this is not C\n"), 1, "does not compile")
if failed:
    sys.exit("FAIL: " + "\n".join(failed))
print("check_lv_layout_test: ok")
