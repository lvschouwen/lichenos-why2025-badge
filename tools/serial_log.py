# Badge console logger for the flash host. Runs there, not on the PC: tools/badge.sh logger install copies it to
# ~/badge/serial_log.py and installs the udev rule + service that start it when the badge connects (#276). It does NOT reset the badge (CH340: EN on RTS, BOOT on DTR; same open order as
# tools/badge.sh console), appends every line with a wall-clock timestamp to a file (tail -f it), reopens the
# port when the cable is moved and keeps the file under ~50 MB (moves it to .1).
#
# Pause for flashing (#61): while <log>.pause exists the logger closes the port, creates <log>.paused and waits;
# tools/badge.sh creates .pause around every esptool or console use and removes it afterwards. A .pause older than
# PAUSE_MAX_S is treated as left over from a crashed flash and ignored, so the log always comes back.
#
# Re-enumeration (#115): after a CH340 drop udev sometimes leaves the by-id link missing for minutes. The logger then
# opens the ttyUSB node whose driver is ch341-uart instead (never another adapter on the host), and writes one line
# when the port is lost and one when it is back (with the node it opened), so gaps in the log are visible.
#
# Usage: serial_log.py [port] [~/badge-logs/badge-serial.log]
import glob
import os
import sys
import time

BY_ID = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"  # survives a replug that gives a new ttyUSB number
CH340_DRIVER = "ch341-uart"
MAX = 50 * 1024 * 1024
PAUSE_MAX_S = 600
RETRY_S = 1


def pick_port(
    preferred: str, preferred_exists: bool, nodes: list[tuple[str, str]]
) -> tuple[str | None, str]:
    """The device to open: the preferred path while it exists, else the only CH340 node.

    nodes: [(dev_path, driver_name)] of the host's ttyUSB devices. Returns (path, how) or (None, why)."""
    if preferred_exists:
        return preferred, "by-id"
    ch340 = sorted(dev for dev, driver in nodes if driver == CH340_DRIVER)
    if len(ch340) == 1:
        return ch340[0], "fallback"
    if not ch340:
        return None, "no CH340 on the host"
    return None, "%d CH340 nodes, not guessing" % len(ch340)


def tty_usb_nodes() -> list[tuple[str, str]]:
    nodes = []
    for sysdir in glob.glob("/sys/class/tty/ttyUSB*"):
        try:
            driver = os.path.basename(os.path.realpath(os.path.join(sysdir, "device", "driver")))
        except OSError:
            continue
        nodes.append(("/dev/" + os.path.basename(sysdir), driver))
    return nodes


def outage_text(lost_at: float, now: float, node: str, how: str) -> str:
    return "--- port back after %.0f s: %s (%s)" % (now - lost_at, node, how)


def stamp() -> str:
    return time.strftime("%H:%M:%S ")


def main() -> None:
    import serial  # only on the flash host; the tests import this file without pyserial

    port = sys.argv[1] if len(sys.argv) > 1 else BY_ID
    path = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/badge-logs/badge-serial.log")
    os.makedirs(os.path.dirname(path), exist_ok=True)  # outside tmpfs: survives a reboot of the host (#276)
    pause_path, paused_path = path + ".pause", path + ".paused"

    def open_port(dev):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = dev, 115200, 0.2
        s.dsrdtr = True
        s.rts = False
        s.open()
        s.dtr = False
        return s

    def pause_requested():
        try:
            return time.time() - os.path.getmtime(pause_path) < PAUSE_MAX_S
        except OSError:
            return False

    def wait_while_paused(out):
        open(paused_path, "w").close()
        out.write(stamp() + "--- paused: port released for flashing\n")
        while pause_requested():
            time.sleep(0.1)
        try:
            os.remove(paused_path)
        except OSError:
            pass
        out.write(stamp() + "--- resumed\n")

    out = open(path, "a", buffering=1)
    try:
        os.remove(paused_path)  # stale marker from a previous run
    except OSError:
        pass

    buf = b""
    lost_at = None  # wall time of the first failure of the current outage
    while True:
        if pause_requested():
            wait_while_paused(out)
        dev, how = pick_port(port, os.path.exists(port), tty_usb_nodes())
        try:
            if dev is None:
                raise OSError(how)
            s = open_port(dev)
            node = os.path.realpath(dev)
            if lost_at is None:
                out.write(stamp() + "--- port open %s: %s (%s)\n" % (port, node, how))
            else:
                out.write(stamp() + outage_text(lost_at, time.time(), node, how) + "\n")
                lost_at = None
            while not pause_requested():
                buf += s.read(4096)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    out.write(stamp() + line.rstrip(b"\r").decode("utf-8", "replace") + "\n")
                if out.tell() > MAX:
                    out.close()
                    os.replace(path, path + ".1")
                    out = open(path, "a", buffering=1)
            s.close()
        except (serial.SerialException, OSError) as e:
            if buf:
                out.write(stamp() + buf.rstrip(b"\r").decode("utf-8", "replace") + "\n")
                buf = b""
            if lost_at is None:  # one line per outage, not one per retry
                lost_at = time.time()
                out.write(stamp() + "--- port lost (%s), retrying\n" % e)
            time.sleep(RETRY_S)


if __name__ == "__main__":
    main()
