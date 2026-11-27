#!/usr/bin/env python3
"""Run a RAM image on the badge through the P4 ROM download mode and print its UART output (#608).

Runs on the flash host (badge.sh load-ram copies it there, with the serial logger paused). esptool enters download
mode, loads the image segments into SRAM and jumps to the entry; the output is read on the same port handle, because
a fresh open resets the badge. Ends at a line 'END ...' or after the time limit, then hard-resets into the active
slot. Nothing is written to flash.

    load_ram_run.py <port> <image.bin> <seconds> [<baud>] [--follow]

--follow ignores the END line and runs to the time limit; SIGINT, SIGHUP and SIGTERM (Ctrl-C, a dropped connection)
still reach the hard reset below.

The serial logger has the port closed while this runs, so the output is also appended, as whole lines with the
logger's 'HH:MM:SS ' prefix and between '--- v3 load-ram' marker lines, to ~/badge-logs/badge-serial.log (when that
directory exists): the owner's log shows what ran.
"""
import os
import re
import signal
import sys
import time

LOG = "~/badge-logs/badge-serial.log"


def stamp() -> str:
    return time.strftime("%H:%M:%S ")  # serial_log.py's prefix


class LogMirror:
    """Appends whole UART lines to the logger's file; a partial last line is written by close()."""

    def __init__(self, f, clock=stamp):
        self.f, self.clock, self.part = f, clock, b""

    def marker(self, text: str) -> None:
        self.f.write(f"{self.clock()}--- v3 load-ram: {text}\n")
        self.f.flush()

    def feed(self, chunk: bytes) -> None:
        *lines, self.part = (self.part + chunk).split(b"\n")
        for line in lines:
            self.write(line)
        if lines:
            self.f.flush()

    def write(self, line: bytes) -> None:
        self.f.write(self.clock() + line.rstrip(b"\r").decode("utf-8", "replace") + "\n")

    def close(self, text: str) -> None:
        if self.part:
            self.write(self.part)
            self.part = b""
        self.marker(text)
        self.f.close()


def open_mirror(path: str = LOG):
    path = os.path.expanduser(path)
    if not os.path.isdir(os.path.dirname(path)):
        return None
    try:
        return LogMirror(open(path, "a", encoding="utf-8"))
    except OSError as e:
        print(f"load-ram: not mirroring to {path}: {e}", file=sys.stderr)
        return None


def main() -> int:
    from esptool.cmds import detect_chip, load_ram

    args = [a for a in sys.argv[1:] if a != "--follow"]
    follow = len(args) != len(sys.argv) - 1
    if len(args) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    port, image, secs = args[0], args[1], int(args[2] or 20)
    baud = int(args[3]) if len(args) == 4 else 115200
    for sig in (signal.SIGHUP, signal.SIGTERM):
        signal.signal(sig, signal.default_int_handler)
    esp = detect_chip(port, baud, "default-reset")
    if esp.CHIP_NAME != "ESP32-P4":
        print(f"refusing: chip on {port} is {esp.CHIP_NAME}, not ESP32-P4", file=sys.stderr)
        esp._port.close()
        return 1
    rc = 3
    mirror = open_mirror()
    if mirror:
        mirror.marker(f"start ({os.path.basename(image)}, {secs} s{', follow' if follow else ''})")
    try:
        load_ram(esp, image)
        ser = esp._port
        ser.timeout = 0.2
        buf = bytearray()
        end = time.monotonic() + secs
        while time.monotonic() < end:
            chunk = ser.read(max(1, ser.in_waiting))
            if not chunk:
                continue
            buf += chunk
            if mirror:
                mirror.feed(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if not follow and re.search(rb"(^|\n)END [^\r\n]*\r?\n", buf):
                rc = 0
                break
        if rc and follow:
            rc = 0
            print(f"\nload-ram: followed for {secs} s", file=sys.stderr)
        elif rc:
            print(f"\nload-ram: no END line within {secs} s (watchdog reset or hang?)", file=sys.stderr)
    except KeyboardInterrupt:
        rc = 0 if follow else 3
        print("\nload-ram: interrupted", file=sys.stderr)
    finally:
        if mirror:
            mirror.close(f"end (rc {rc}), hard reset")
        time.sleep(0.2)
        esp.hard_reset()
        esp._port.close()
        print("load-ram: hard reset sent, the badge boots its active slot", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
