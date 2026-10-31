#!/usr/bin/env python3
"""Generates Lichen's UI fonts (#16): firmware/assets/fonts/lichen_<size>.bin, LVGL binary fonts for the assets partition.

  tools/make_fonts.py [--cache DIR]

Each size merges four sources with lv_font_conv (the first font that has a code point wins):
  Montserrat Medium  Latin, Latin-1/Extended-A/B, Cyrillic, general punctuation, currency signs (the UI's look)
  Font Awesome       the LV_SYMBOL_* icons, same code points as LVGL's own lv_font_montserrat_24.c
  DejaVu Sans        Greek, arrows, geometric shapes, zero width space
  Noto Emoji         monochrome emoji (contact names carry them), plus the zero-width joiner and variation selectors

The sources are downloaded once into --cache (default tools/.font-cache, git-ignored) and checked against the sha256
pinned below. The generated .bin files are committed, so a normal build needs neither network nor node.
The firmware loads them at boot (components/lichen/fonts.c); host_tests/font_cover_test.c checks their coverage.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

LV_FONT_CONV = "lv_font_conv@1.5.3"
LVGL_RAW = "https://raw.githubusercontent.com/lvgl/lvgl/v9.6.0/scripts/generators/built_in_font/"
NOTO_RAW = "https://raw.githubusercontent.com/google/fonts/6c1dbe27dcf7d817d4b62cec033ba77fa8e2a5ec/ofl/notoemoji/"

# name in the cache -> (url, sha256)
SOURCES = {
    "Montserrat-Medium.ttf": (
        LVGL_RAW + "Montserrat-Medium.ttf",
        "421f26b23e2be6b98373d32acd3cb2897b154d4bf0a77d26534ce476e4cbed53",
    ),
    "FontAwesome5-Solid+Brands+Regular.woff": (
        LVGL_RAW + "FontAwesome5-Solid+Brands+Regular.woff",
        "f4e42f6cd69e5dbdcccc0f2f5be136cebde0e427641e45403bf9173a92da95f4",
    ),
    "DejaVuSans.ttf": (
        LVGL_RAW + "DejaVuSans.ttf",
        "3fdf69cabf06049ea70a00b5919340e2ce1e6d02b0cc3c4b44fb6801bd1e0d22",
    ),
    "NotoEmoji[wght].ttf": (
        NOTO_RAW + "NotoEmoji%5Bwght%5D.ttf",
        "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551",
    ),
}

# The LV_SYMBOL_* code points: the "Opts" line of LVGL's lv_font_montserrat_24.c
SYMBOLS = (
    "61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,"
    "61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,"
    "61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,"
    "62087,62099,62212,62189,62810,63426,63650"
)
LATIN = "0x20-0x7E,0xA0-0x24F,0x400-0x4FF,0x2000-0x206F,0x20A0-0x20CF"
DEJAVU = "0x200B,0x370-0x3FF,0x2190-0x21FF,0x25A0-0x25FF"
# Includes 0x1F900-0x1F9FF (🤖 🦋 🦎 🦫 are in node names); 0x200D/0xFE0E/0xFE0F are zero-width in this font
EMOJI = "0x200D,0xFE0E,0xFE0F,0x2300-0x23FF,0x2600-0x27BF,0x2B00-0x2BFF,0x1F000-0x1FAFF"

# size -> whether it carries the emoji. RLE-compressed (needs LV_USE_FONT_COMPRESSED): 40 px with emoji is 1.5 MB raw
SIZES = {24: True, 28: True, 40: True}


def fetch(cache: Path) -> None:
    cache.mkdir(parents=True, exist_ok=True)
    for name, (url, digest) in SOURCES.items():
        path = cache / name
        if not path.exists():
            print(f"downloading {url}")
            with urllib.request.urlopen(url) as resp:
                path.write_bytes(resp.read())
        got = hashlib.sha256(path.read_bytes()).hexdigest()
        if got != digest:
            sys.exit(f"{path}: sha256 {got}, expected {digest}")


def convert(cache: Path, size: int, emoji: bool, out: Path) -> None:
    cmd = ["npx", "--yes", LV_FONT_CONV, "--size", str(size), "--bpp", "4", "--format", "bin",
           "--font", str(cache / "Montserrat-Medium.ttf"), "-r", LATIN,
           "--font", str(cache / "FontAwesome5-Solid+Brands+Regular.woff"), "-r", SYMBOLS,
           "--font", str(cache / "DejaVuSans.ttf"), "-r", DEJAVU]
    if emoji:
        cmd += ["--font", str(cache / "NotoEmoji[wght].ttf"), "-r", EMOJI]
    cmd += ["-o", str(out)]
    subprocess.run(cmd, check=True)


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description="Generate Lichen's UI fonts")
    parser.add_argument("--cache", type=Path, default=here / ".font-cache")
    parser.add_argument("--out", type=Path, default=here.parent / "firmware" / "assets" / "fonts")
    args = parser.parse_args()
    if not shutil.which("npx"):
        sys.exit("npx (node) is needed to run lv_font_conv")
    fetch(args.cache)
    args.out.mkdir(parents=True, exist_ok=True)
    for size, emoji in SIZES.items():
        out = args.out / f"lichen_{size}.bin"
        convert(args.cache, size, emoji, out)
        print(f"{out.name}: {out.stat().st_size} bytes")


if __name__ == "__main__":
    main()
