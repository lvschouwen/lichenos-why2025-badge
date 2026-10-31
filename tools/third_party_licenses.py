#!/usr/bin/env python3
"""Builds THIRD_PARTY_LICENSES.txt: the license texts of the third-party code linked into Lichen's two images.

MIT, BSD, Zlib, ISC and OFL ask for their notice in every copy, binaries included, so this file ships next to every
released image. The list below is what the images LINK (checked with nm against the built ELFs, #18), not
everything the build compiles: LVGL compiles all its bundled libraries but only its printf is enabled, and ESP-IDF
components such as argtable3, linenoise, SPIFFS or OpenThread are not linked.

  tools/third_party_licenses.py           regenerate the file
  tools/third_party_licenses.py --check   fail if it is out of date (firmware/build.sh runs this after a build)

Sources are read from the repo, firmware/managed_components, c6/managed_components and ESP-IDF (BADGE_IDF_PATH),
so a build must have fetched the managed components first. Update the list when a dependency is added or enabled.
"""

import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IDF = Path(os.path.expanduser(os.environ.get("BADGE_IDF_PATH", "~/esp/esp-idf")))
OUT = REPO / "THIRD_PARTY_LICENSES.txt"

ROOTS = {
    "repo": REPO,
    "idf": IDF,
    "fw": REPO / "firmware" / "managed_components",
    "c6": REPO / "c6" / "managed_components",
}

BSD_2 = """Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
   following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""

BSD_3 = BSD_2.replace(
    "\n\nTHIS SOFTWARE",
    "\n3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote\n"
    "   products derived from this software without specific prior written permission.\n\nTHIS SOFTWARE",
    1,
)

# (name, url, images, license, source). Source is ("file", root, path) for a license file, ("header", root, path)
# for the leading comment of a source file, or ("text", str) when upstream only states the license (SPDX line or
# ESP-IDF's COPYRIGHT.rst) and the standard text is filled in with the stated copyright holder.
ENTRIES = [
    ("MeshCore", "https://github.com/meshcore-dev/MeshCore", "P4", "MIT",
     ("file", "repo", "meshcore/license.txt")),
    ("ed25519 (orlp, in MeshCore)", "https://github.com/orlp/ed25519", "P4", "Zlib",
     ("file", "repo", "meshcore/lib/ed25519/license.txt")),
    ("rweather/arduinolibs Crypto", "https://github.com/rweather/arduinolibs", "P4", "MIT",
     ("file", "repo", "third_party/arduinolibs/libraries/LICENSE.txt")),
    ("base64_arduino", "https://github.com/Densaugeo/base64_arduino", "P4", "MIT",
     ("file", "repo", "third_party/base64/LICENSE")),
    ("tomlc17", "https://github.com/cktan/tomlc17", "P4", "MIT",
     ("file", "repo", "firmware/components/lora/thirdparty/toml-LICENSE")),
    ("esp_tca8418", "https://github.com/atanisoft/esp_tca8418", "P4", "BSD-2-Clause",
     ("file", "repo", "firmware/components/esp_tca8418/LICENSE")),
    ("LVGL", "https://github.com/lvgl/lvgl", "P4", "MIT",
     ("file", "fw", "lvgl__lvgl/LICENCE.txt")),
    ("printf by Marco Paland (in LVGL)", "https://github.com/mpaland/printf", "P4", "MIT",
     ("header", "fw", "lvgl__lvgl/src/stdlib/builtin/lv_sprintf_builtin.c")),
    ("Montserrat font (in LVGL)", "https://github.com/JulietaUla/Montserrat", "P4", "OFL-1.1",
     ("file", "fw", "lvgl__lvgl/scripts/generators/built_in_font/font_license/Montserrat/OFL.txt")),
    ("Font Awesome 5 Free symbols (in LVGL's fonts)", "https://fontawesome.com", "P4", "OFL-1.1 (fonts)",
     ("file", "fw", "lvgl__lvgl/scripts/generators/built_in_font/font_license/FontAwesome5/LICENSE.txt")),
    ("DejaVu Sans font (Lichen's fonts, from LVGL's font sources)", "https://dejavu-fonts.github.io", "P4",
     "Bitstream Vera license, public domain changes",
     ("file", "fw", "lvgl__lvgl/scripts/generators/built_in_font/font_license/DejaVuSans/LICENSE")),
    ("Noto Emoji font (Lichen's fonts)", "https://github.com/googlefonts/noto-emoji", "P4", "OFL-1.1",
     ("file", "repo", "third_party/noto-emoji/OFL.txt")),
    ("RadioLib", "https://github.com/jgromes/RadioLib", "C6", "MIT",
     ("file", "c6", "jgromes__radiolib/license.txt")),
    ("FreeRTOS kernel (in ESP-IDF)", "https://www.freertos.org", "P4, C6", "MIT",
     ("file", "idf", "components/freertos/FreeRTOS-Kernel/LICENSE.md")),
    ("lwIP (in ESP-IDF)", "https://savannah.nongnu.org/projects/lwip/", "P4, C6", "BSD-3-Clause",
     ("file", "idf", "components/lwip/lwip/COPYING")),
    ("TLSF allocator (in ESP-IDF)", "https://github.com/espressif/tlsf", "P4, C6", "BSD-3-Clause",
     ("text", "Copyright (c) 2006-2016, Matthew Conte\nAll rights reserved.\n\n" + BSD_3)),
    ("FatFs (in ESP-IDF)", "http://elm-chan.org/fsw/ff/", "P4, C6", "FatFs license (BSD-1-Clause style)",
     ("header", "idf", "components/fatfs/src/ff.h")),
    ("SD/MMC driver, from OpenBSD (in ESP-IDF)", "https://github.com/openbsd/src", "P4", "ISC",
     ("header", "idf", "components/sdmmc/sdmmc_common.c")),
    ("protobuf-c (in ESP-IDF and ESP-Hosted)", "https://github.com/protobuf-c/protobuf-c", "P4, C6", "BSD-2-Clause",
     ("file", "idf", "components/protobuf-c/protobuf-c/LICENSE")),
    ("Picolibc C library (ESP-IDF toolchain)", "https://keithp.com/picolibc/", "P4, C6", "BSD and others, see text",
     ("file", "idf", "components/esp_libc/COPYING.picolibc")),
    ("wpa_supplicant (in ESP-IDF)", "https://w1.fi/wpa_supplicant/", "C6", "BSD-3-Clause",
     ("file", "idf", "components/wpa_supplicant/README")),
    ("FreeBSD net80211 (in Espressif's Wi-Fi library)", "https://github.com/freebsd/freebsd-src", "C6",
     "BSD-2-Clause",
     ("text", "Copyright (c) 2004-2008 Sam Leffler, Errno Consulting\nAll rights reserved.\n\n" + BSD_2)),
    ("micro-ecc (in Espressif's Bluetooth controller)", "https://github.com/kmackay/micro-ecc", "C6", "BSD-2-Clause",
     ("file", "idf", "components/bootloader/subproject/components/micro-ecc/micro-ecc/LICENSE.txt")),
]

# Apache-2.0 code needs the license text once; none of these ships a NOTICE file (checked in #18).
APACHE = [
    ("ESP-IDF (Espressif's own code, Wi-Fi/Bluetooth/PHY libraries, Mbed TLS, NimBLE)", "P4, C6"),
    ("ESP-Hosted MCU (our fork; dual GPL-2.0-only OR Apache-2.0 files used under Apache-2.0)", "P4, C6"),
    ("esp_lcd_st7703", "P4"),
    ("esp_lvgl_port, esp-serial-flasher, esp_serial_slave_link, esp_wifi_remote, eppp_link, i2c_bus,"
     " usb, usb_host_cdc_acm, usb_host_ch34x_vcp, usb_host_cp210x_vcp", "P4"),
]
APACHE_TEXT = ("idf", "LICENSE")

HEADER = """Third-party licenses for Lichen OS
==================================

Lichen is free software under the GNU General Public License version 3 (see COPYING). Its source code, including the
code of every third-party library listed here, is at https://github.com/lvschouwen/lichenos-why2025-badge.

The C6 image is a separate program on its own chip. Its own code (c6/main and shared/lora) is under the Apache
License 2.0, as marked in each file, so it can link Espressif's binary-only Wi-Fi, Bluetooth and PHY libraries.

A Lichen release has two images: P4 (the Lichen firmware, badge_fw.bin) and C6 (the radio coprocessor image, which
the P4 image carries and flashes onto the C6). This file lists the third-party code linked into each image and
reproduces its license. Generated by tools/third_party_licenses.py; do not edit by hand.

"""


def read(root: str, rel: str) -> str:
    path = ROOTS[root] / rel
    if not path.is_file():
        sys.exit(f"{path} is missing: build once (managed components) and set BADGE_IDF_PATH")
    return path.read_text(encoding="utf-8", errors="strict")


def leading_comment(text: str) -> str:
    """The license comment at the top of a source file, without comment markers."""
    lines = text.splitlines()
    out = []
    if lines[0].lstrip().startswith("//"):
        for line in lines:
            if not line.lstrip().startswith("//"):
                break
            out.append(line.lstrip()[2:].lstrip("/").removeprefix(" "))
    else:
        for line in lines:
            done = "*/" in line
            stripped = line.split("*/")[0].strip()
            stripped = stripped.removeprefix("/*").lstrip("*/").removeprefix(" ").rstrip(" /")
            if not stripped.strip("-"):
                stripped = ""  # FatFs-style box rules
            out.append(stripped)
            if done:
                break
    while out and not out[0].strip("-/ "):
        out.pop(0)
    while out and not out[-1].strip("-/ "):
        out.pop()
    return "\n".join(line.rstrip() for line in out) + "\n"


def body(source: tuple) -> str:
    kind = source[0]
    if kind == "file":
        return read(source[1], source[2])
    if kind == "header":
        return leading_comment(read(source[1], source[2]))
    return source[1]


def generate() -> str:
    rule = "-" * 118 + "\n"
    parts = [HEADER, "Contents\n--------\n\n"]
    for name, _, images, lic, _ in ENTRIES:
        parts.append(f"  {name:<62} {images:<8} {lic}\n")
    for name, images in APACHE:
        parts.append(f"  {name}\n  {'':<62} {images:<8} Apache-2.0\n")
    parts.append("\n")
    for name, url, images, lic, src in ENTRIES:
        parts += [rule, f"{name}\n{url}\nImages: {images}    License: {lic}\n", rule, "\n",
                  body(src).rstrip() + "\n\n"]
    parts += [rule, "Apache License 2.0, for:\n"]
    parts += [f"  - {name} ({images})\n" for name, images in APACHE]
    parts += [rule, "\n", read(*APACHE_TEXT).rstrip() + "\n"]
    return "".join(parts)


def main() -> None:
    text = generate()
    if "--check" in sys.argv[1:]:
        if not OUT.is_file() or OUT.read_text(encoding="utf-8") != text:
            sys.exit(f"{OUT.name} is out of date: run tools/third_party_licenses.py and commit it")
        print(f"{OUT.name} is up to date")
        return
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT} ({text.count(chr(10))} lines)")


if __name__ == "__main__":
    main()
