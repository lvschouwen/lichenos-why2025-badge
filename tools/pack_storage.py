#!/usr/bin/env python3
"""LittleFS images of the badge's /storage drive (#300), with exactly the badge's on-disk config.

  pack <src dir> <out.bin>     every file and folder under src dir into a fresh image the size of the partition
  list <image>                 one line per entry: sha256, size and path (folders: "-" and a trailing "/")
  verify <image> <src dir>     the image holds exactly src dir (same entries, same SHA-256); exit 1 if not
  extract <image> <dst dir>    the image's files into dst dir (which must not exist yet)
  check-config <sdkconfig.json> <esp_littlefs dir>
                               the built firmware's LittleFS config and littlefs disk version match CONFIG below
  selftest                     a tree with nested and empty folders and long names packs, lists and extracts back

The build makes storage.bin with "pack" (firmware/main/CMakeLists.txt) and checks it with "verify" and "check-config"
(firmware/build.sh). The USB move of a FAT badge to LittleFS (#300) packs the files taken off the old drive with the
same command: tools/pack_storage.sh runs this script in its pinned littlefs-python environment.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import tempfile

from littlefs import LittleFS, LittleFSError, UserContext

PARTITION_SIZE = 0x4C0000  # firmware/partitions.csv, storage
# The badge's lfs_config, from esp_littlefs 1.23.1 (components/storage/idf_component.yml) and the CONFIG_LITTLEFS_*
# values in firmware/sdkconfig.defaults. name_max: esp_littlefs leaves cfg.name_max 0, so littlefs uses LFS_NAME_MAX
# (255) and a drive the badge formats records 255; file_max and attr_max are littlefs's defaults the same way.
CONFIG = {
    "block_size": 4096,  # the flash erase sector; esp_littlefs's CONFIG_LITTLEFS_BLOCK_SIZE
    "read_size": 128,
    "prog_size": 128,
    "cache_size": 512,
    "lookahead_size": 128,
    "block_cycles": 512,
    "name_max": 255,
    "file_max": 0,
    "attr_max": 0,
    "disk_version": 0x00020001,  # littlefs 2.1 on disk, what esp_littlefs writes without CONFIG_LITTLEFS_MULTIVERSION
}
SDKCONFIG = {  # CONFIG key -> sdkconfig.json name
    "read_size": "LITTLEFS_READ_SIZE",
    "prog_size": "LITTLEFS_WRITE_SIZE",
    "cache_size": "LITTLEFS_CACHE_SIZE",
    "lookahead_size": "LITTLEFS_LOOKAHEAD_SIZE",
    "block_cycles": "LITTLEFS_BLOCK_CYCLES",
}
LFS_TYPE_DIR = 2  # lfs.h, as LFSStat.type reports a folder
MTIME_ATTR = "t"  # esp_littlefs's ESP_LITTLEFS_ATTR_MTIME: a little-endian 64-bit time_t
# Files up to inline_max bytes live inside their folder's metadata pair instead of taking a 4 KB block of their own.
# Neither esp_littlefs nor littlefs-python sets cfg.inline_max, so littlefs 2.11 (lfs_init) derives it the same way on
# both sides: min(cache_size, attr_max (1022, LFS_ATTR_MAX), block_size / 8) = 512. MeshCore's advert blobs in
# /storage/meshcore/bl/ (up to 255 B each, hundreds of them, rewritten on every advert) need it at 255 or more.
LFS_ATTR_MAX = 1022
INLINE_MAX = min(CONFIG["cache_size"], CONFIG["attr_max"] or LFS_ATTR_MAX, CONFIG["block_size"] // 8)
INLINE_NEEDED = 255


def mount(context=None, block_count=PARTITION_SIZE // CONFIG["block_size"], fresh=False):
    if context is None:
        context = UserContext(block_count * CONFIG["block_size"])  # erased flash: 0xFF
    fs = LittleFS(context=context, mount=False, block_count=block_count, **CONFIG)
    if fresh:
        fs.format()
    fs.mount()  # an image that does not mount is an error here, never formatted
    return fs


def open_image(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % CONFIG["block_size"]:
        sys.exit(f"{path}: {len(data)} bytes is not a whole number of {CONFIG['block_size']} byte blocks")
    context = UserContext(len(data))
    context.buffer[:] = data
    return mount(context, len(data) // CONFIG["block_size"])


def host_entries(src):
    """{path: (sha256 or None for a folder, size)} of everything under src, paths relative and '/'-separated"""
    out = {}
    for root, dirs, files in os.walk(src, followlinks=False):
        dirs.sort()
        rel_root = os.path.relpath(root, src).replace(os.sep, "/")
        rel_root = "" if rel_root == "." else rel_root + "/"
        for d in dirs:
            if os.path.islink(os.path.join(root, d)):
                sys.exit(f"{os.path.join(root, d)}: a symlink; the drive has none")
            out[rel_root + d] = (None, 0)
        for name in sorted(files):
            full = os.path.join(root, name)
            if os.path.islink(full) or not os.path.isfile(full):
                sys.exit(f"{full}: not a regular file")
            with open(full, "rb") as f:
                data = f.read()
            out[rel_root + name] = (hashlib.sha256(data).hexdigest(), len(data))
    return out


def image_entries(fs, top=""):
    out = {}
    for info in sorted(fs.scandir(top or "/"), key=lambda i: i.name):
        rel = top + info.name
        if info.type == LFS_TYPE_DIR:
            out[rel] = (None, 0)
            out.update(image_entries(fs, rel + "/"))
        else:
            with fs.open(rel, "rb") as f:
                data = f.read()
            out[rel] = (hashlib.sha256(data).hexdigest(), len(data))
    return out


def check_names(entries):
    for rel in entries:
        for part in rel.split("/"):
            if len(part.encode()) > CONFIG["name_max"]:
                sys.exit(f"{rel}: the name {part!r} is over {CONFIG['name_max']} bytes")


def pack(src, out):
    if not os.path.isdir(src):
        sys.exit(f"{src}: not a folder")
    entries = host_entries(src)
    check_names(entries)
    fs = mount(fresh=True)
    try:
        for rel, (digest, _) in entries.items():  # os.walk order: a folder before what is in it
            full = os.path.join(src, *rel.split("/"))
            if digest is None:
                fs.mkdir(rel)
                continue
            with open(full, "rb") as f:
                data = f.read()
            with fs.open(rel, "wb") as f:
                f.write(data)
            fs.setattr(rel, MTIME_ATTR, struct.pack("<q", int(os.stat(full).st_mtime)))
    except LittleFSError as e:
        sys.exit(f"{src}: does not fit in a {PARTITION_SIZE} byte drive ({e})")
    used = fs.used_block_count
    fs.unmount()
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(fs.context.buffer)
    os.replace(tmp, out)
    files = sum(1 for d, _ in entries.values() if d)
    print(f"{out}: {files} files, {len(entries) - files} folders, {used} of {PARTITION_SIZE // CONFIG['block_size']} "
          f"blocks used")


def list_lines(entries):
    for rel, (digest, size) in entries.items():
        yield f"{digest}  {size:8d}  {rel}" if digest else f"{'-' * 64}  {'-':>8}  {rel}/"


def verify_problems(image, src):
    want = host_entries(src)
    got = image_entries(open_image(image))
    bad = [f"missing in the image: {p}" for p in want if p not in got]
    bad += [f"not in {src}: {p}" for p in got if p not in want]
    bad += [f"differs: {p}" for p in want if p in got and want[p] != got[p]]
    return bad, len(got)


def verify(image, src):
    bad, n = verify_problems(image, src)
    for line in bad:
        print(f"{image}: {line}", file=sys.stderr)
    if bad:
        sys.exit(1)
    print(f"{image}: {n} entries, every file's SHA-256 matches {src}")


def extract(image, dst):
    fs = open_image(image)
    entries = image_entries(fs)
    os.makedirs(dst)
    for rel, (digest, _) in entries.items():
        full = os.path.join(dst, *rel.split("/"))
        if digest is None:
            os.makedirs(full, exist_ok=True)
            continue
        with fs.open(rel, "rb") as f, open(full, "wb") as out:
            out.write(f.read())
    print(f"{dst}: {len(entries)} entries")


def check_config(sdkconfig_json, component_dir):
    with open(sdkconfig_json) as f:
        cfg = json.load(f)
    bad = [f"{name}={cfg.get(name)}, pack_storage.py has {CONFIG[key]}" for key, name in SDKCONFIG.items()
           if cfg.get(name) != CONFIG[key]]
    if cfg.get("LITTLEFS_MULTIVERSION"):
        bad.append("LITTLEFS_MULTIVERSION is on: the disk version is no longer littlefs's own")
    if cfg.get("LITTLEFS_MMAP_PARTITION"):
        bad.append("LITTLEFS_MMAP_PARTITION is on: no flash mapping at runtime (#31)")
    if INLINE_MAX < INLINE_NEEDED:
        bad.append(f"inline_max {INLINE_MAX} < {INLINE_NEEDED}: every advert blob would take a whole block")
    with open(os.path.join(component_dir, "src", "esp_littlefs.c")) as f:
        if "inline_max" in f.read():
            bad.append("esp_littlefs.c sets inline_max itself now: pack_storage.py's INLINE_MAX may no longer match")
    lfs_h = os.path.join(component_dir, "src", "littlefs", "lfs.h")
    with open(lfs_h) as f:
        m = re.search(r"#define LFS_DISK_VERSION (0x[0-9a-fA-F]+)", f.read())
    if not m or int(m.group(1), 16) != CONFIG["disk_version"]:
        bad.append(f"{lfs_h}: LFS_DISK_VERSION {m.group(1) if m else '?'}, pack_storage.py has "
                   f"{CONFIG['disk_version']:#010x}")
    for line in bad:
        print(f"pack_storage check-config: {line}", file=sys.stderr)
    if bad:
        sys.exit(1)
    print(f"pack_storage check-config: the firmware's LittleFS config matches storage.bin's (inline_max {INLINE_MAX})")


def blobs_selftest(tmp):
    """500 advert-blob-sized files (140-255 B) in one folder: inline, so far fewer blocks than files"""
    src = os.path.join(tmp, "blobs")
    bl = os.path.join(src, "meshcore", "bl")
    os.makedirs(bl)
    for i in range(500):
        with open(os.path.join(bl, f"{i:016x}"), "wb") as f:
            f.write(os.urandom(140 + i % 116))
    image = os.path.join(tmp, "blobs.bin")
    pack(src, image)
    verify(image, src)
    used = open_image(image).used_block_count
    print(f"pack_storage selftest: 500 blobs of 140-255 B take {used} blocks ({used * CONFIG['block_size']} B)")
    assert used < 250, f"{used} blocks for 500 small files: they are not inline"


def selftest():
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "src")
        files = {
            "lora.toml": b"[lora]\nfreq = 869.618\n",
            "c6/firmware.bin": os.urandom(300 * 1024),  # several blocks
            "c6/firmware.bin.md5": b"0123456789abcdef0123456789abcdef",
            "meshcore/" + "n" * 200 + ".dat": os.urandom(777),  # a long name, no 8.3 alias anywhere
            "meshcore/Contacts3": os.urandom(5000),
            "meshcore/contacts3": b"case is kept",  # LittleFS names are case-sensitive
            "deep/a/b/c/empty.txt": b"",
        }
        for rel, data in files.items():
            os.makedirs(os.path.dirname(os.path.join(src, rel)), exist_ok=True)
            with open(os.path.join(src, rel), "wb") as f:
                f.write(data)
        os.makedirs(os.path.join(src, "bench", "empty"))
        image = os.path.join(tmp, "storage.bin")
        pack(src, image)
        assert os.path.getsize(image) == PARTITION_SIZE
        verify(image, src)
        fs = open_image(image)
        stat = fs.fs_stat()
        assert (stat.block_size, stat.block_count, stat.name_max, stat.disk_version) == (
            CONFIG["block_size"], PARTITION_SIZE // CONFIG["block_size"], CONFIG["name_max"], CONFIG["disk_version"]
        ), stat
        assert len(fs.getattr("lora.toml", MTIME_ATTR)) == 8
        blobs_selftest(tmp)
        back = os.path.join(tmp, "back")
        extract(image, back)
        assert host_entries(back) == host_entries(src)
        with open(os.path.join(src, "lora.toml"), "ab") as f:  # verify must notice a changed file
            f.write(b"#")
        assert verify_problems(image, src)[0] == ["differs: lora.toml"], "verify missed a changed file"
        os.remove(os.path.join(src, "deep", "a", "b", "c", "empty.txt"))
        assert "not in " + src + ": deep/a/b/c/empty.txt" in verify_problems(image, src)[0], "verify missed a file"
    print("pack_storage selftest: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, args in (("pack", ("src", "out")), ("list", ("image",)), ("verify", ("image", "src")),
                       ("extract", ("image", "dst")), ("check-config", ("sdkconfig_json", "component_dir")),
                       ("selftest", ())):
        p = sub.add_parser(name)
        for a in args:
            p.add_argument(a)
    a = ap.parse_args()
    if a.cmd == "pack":
        pack(a.src, a.out)
    elif a.cmd == "list":
        for line in list_lines(image_entries(open_image(a.image))):
            print(line)
    elif a.cmd == "verify":
        verify(a.image, a.src)
    elif a.cmd == "extract":
        extract(a.image, a.dst)
    elif a.cmd == "check-config":
        check_config(a.sdkconfig_json, a.component_dir)
    else:
        selftest()


if __name__ == "__main__":
    main()
