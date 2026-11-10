"""Host test (#294): every folder the OS keeps directly under /storage or /sd (`STORAGE_BASE "/x"`, `SD_BASE "/x"` and
the gate's LICHEN_SYS_ copies of #286, in firmware/ and shared/) is a reserved app id in lichen_api/fs_path.c, so no app
can be handed an OS folder as its own data folder (app folders are <base>/<id>)."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
# Not the #define itself; the gate's copies (LICHEN_SYS_STORAGE_BASE, LICHEN_SYS_SD_BASE) count too. A folder is a name
# followed by "/" or the end of the literal: a file at the root ("/ota_token.txt") is not an app id's folder.
FOLDER = re.compile(r'(?<!define )\b(?:LICHEN_SYS_)?(?:STORAGE_BASE|SD_BASE)\s*"/([A-Za-z0-9_-]+)(?=[/"])')
RESERVED = re.compile(r'RESERVED_IDS\[\]\s*=\s*\{([^}]*)\}')


def os_folders():
    found = {}
    for tree in ("firmware/components", "firmware/main", "shared"):
        for path in (ROOT / tree).rglob("*"):
            if path.suffix in (".c", ".h", ".cpp") and path.is_file():
                for name in FOLDER.findall(path.read_text(errors="replace")):
                    found.setdefault(name, path.relative_to(ROOT))
    return found


def reserved_ids():
    text = (ROOT / "firmware/components/lichen_api/fs_path.c").read_text()
    match = RESERVED.search(text)
    if not match:
        sys.exit("FAIL: RESERVED_IDS not found in fs_path.c")
    return set(re.findall(r'"([^"]+)"', match.group(1)))


def main():
    folders = os_folders()
    if "crash" not in folders or "c6" not in folders:
        sys.exit("FAIL: the folder scan found %s, expected at least c6 and crash" % sorted(folders))
    missing = sorted(set(folders) - reserved_ids())
    if missing:
        sys.exit("FAIL: OS folders not reserved as app ids in fs_path.c: %s"
                 % ", ".join("%s (%s)" % (m, folders[m]) for m in missing))
    print("reserved_ids_test: ok (%s)" % ", ".join(sorted(folders)))


if __name__ == "__main__":
    main()
