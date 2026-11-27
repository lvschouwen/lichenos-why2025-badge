#!/usr/bin/env python3
"""Run a RAM image on the badge through the P4 ROM download mode and print its UART output (#608).

Runs on the flash host (badge.sh load-ram copies it there, with the serial logger paused). esptool enters download
mode, loads the image segments into SRAM and jumps to the entry; the output is read on the same port handle, because
a fresh open resets the badge. Ends at a line 'END ...' or after the time limit, then hard-resets into the active
slot. Nothing is written to flash.

    load_ram_run.py <port> <image.bin> <seconds>
"""
import re
import sys
import time

from esptool.cmds import detect_chip, load_ram


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    port, image, secs = sys.argv[1], sys.argv[2], int(sys.argv[3] or 20)
    esp = detect_chip(port, 115200, "default-reset")
    if esp.CHIP_NAME != "ESP32-P4":
        print(f"refusing: chip on {port} is {esp.CHIP_NAME}, not ESP32-P4", file=sys.stderr)
        esp._port.close()
        return 1
    rc = 3
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
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            if re.search(rb"(^|\n)END [^\r\n]*\r?\n", buf):
                rc = 0
                break
        if rc:
            print(f"\nload-ram: no END line within {secs} s (watchdog reset or hang?)", file=sys.stderr)
    finally:
        time.sleep(0.2)
        esp.hard_reset()
        esp._port.close()
        print("load-ram: hard reset sent, the badge boots its active slot", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
