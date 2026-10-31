# Badge console logger for the flash host. Runs there, not on the PC: tools/badge.sh logger-install copies it to
# ~/badge/serial_log.py. It does NOT reset the badge (CH340: EN on RTS, BOOT on DTR; same open order as
# tools/badge.sh console), appends every line with a wall-clock timestamp to a file (tail -f it), reopens the
# port when the cable is moved and keeps the file under ~50 MB (moves it to .1).
#
# Pause for flashing (#61): while <log>.pause exists the logger closes the port, creates <log>.paused and waits;
# tools/badge.sh creates .pause around every esptool or console use and removes it afterwards. A .pause older than
# PAUSE_MAX_S is treated as left over from a crashed flash and ignored, so the log always comes back.
#
# Usage: serial_log.py [port] [/tmp/badge-serial.log]
import os, sys, time, serial

# The by-id name survives a replug that gives the CH340 a new ttyUSB number
port = sys.argv[1] if len(sys.argv) > 1 else "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
path = sys.argv[2] if len(sys.argv) > 2 else "/tmp/badge-serial.log"
pause_path, paused_path = path + ".pause", path + ".paused"
MAX = 50 * 1024 * 1024
PAUSE_MAX_S = 600


def open_port():
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dsrdtr = True
    s.rts = False
    s.open()
    s.dtr = False
    return s


def stamp():
    return time.strftime("%H:%M:%S ")


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
while True:
    if pause_requested():
        wait_while_paused(out)
    try:
        s = open_port()
        out.write(stamp() + "--- port open " + port + "\n")
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
        if buf:
            out.write(stamp() + buf.rstrip(b"\r").decode("utf-8", "replace") + "\n")
            buf = b""
    except (serial.SerialException, OSError) as e:
        out.write(stamp() + "--- port lost (%s), retrying\n" % e)
        time.sleep(2)
