# Badge console logger for the flash host. Runs there, not on the PC: tools/badge.sh logger install copies it to
# ~/badge/serial_log.py and installs the udev rule + service that start it when the badge connects (#276). It does NOT reset the badge (CH340: EN on RTS, BOOT on DTR; same open order as
# tools/badge.sh console), appends every line with a wall-clock timestamp to a file (tail -f it), reopens the
# port when the cable is moved and keeps the file under ~50 MB (moves it to .1).
#
# Console socket (#324): the logger is the only process that opens the port. It serves a Unix socket next to the log
# (badge-console.sock, mode 0600). Protocol, one text line per "\n" both ways:
#   - on connect the logger sends "--- connected <open|paused|lost> <node>" (node: the ttyUSB it has open, or "-");
#   - a client line goes to the UART and into the log as ">> <line>", redacted when it can carry a secret (redact());
#     the other clients see that ">> " line too; while the port is not open the line is refused ("--- refused: ..."),
#     never queued; a line with a control character (tab excepted, one CR before its "\n" is a line end) is refused,
#     not cleaned: badge.sh's on-air gate read the line as typed, so the badge must get exactly that or nothing;
#   - every line read from the UART and every "--- " note goes to the log and to every client; while a secret window
#     is open (a command that prints a secret was sent, SECRET_REPLIES, until its reply is over: secret_over()) every
#     "fw: " line, whoever's command it answers, and a core dump's body go to the clients only, the log gets a count;
#     a failed UART write is refused and never reopens the port (a reopen risks a reset);
#   - a client that does not read is dropped once CLIENT_MAX_BUF bytes wait for it, so it never blocks the logger.
# The logger also writes badge-console.status (clients and port state) for "badge.sh logger status". The same file
# is the client: serial_log.py --send/--session/--monitor <port> ..., exit 3 = the socket is not usable for <port>,
# use the pause path instead (socket_usable()).
#
# Pause for flashing (#61): while <log>.pause exists the logger closes the port, creates <log>.paused and waits;
# tools/badge.sh creates .pause around every esptool use (and around console use when the socket is not usable) and
# removes it afterwards. A .pause older than PAUSE_MAX_S is treated as left over from a crashed flash and ignored, so
# the log always comes back.
#
# Re-enumeration (#115): after a CH340 drop udev sometimes leaves the by-id link missing for minutes. The logger then
# opens the ttyUSB node whose driver is ch341-uart instead (never another adapter on the host), and writes one line
# when the port is lost and one when it is back (with the node it opened), so gaps in the log are visible.
#
# Usage: serial_log.py [port] [~/badge-logs/badge-serial.log]
#        serial_log.py [--b64] --send <port> <line> [seconds] | --session <port> <line> <quiet_s> <max_s> [until]
#                      | --monitor <port>
import base64
import glob
import os
import re
import select
import signal
import socket
import sys
import time

BY_ID = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"  # survives a replug that gives a new ttyUSB number
CH340_DRIVER = "ch341-uart"
MAX = 50 * 1024 * 1024
PAUSE_MAX_S = 600
RETRY_S = 1
SOCK_NAME = "badge-console.sock"
STATUS_NAME = "badge-console.status"
DEFAULT_LOG = "~/badge-logs/badge-serial.log"
CLIENT_MAX_BUF = 256 * 1024  # about 20 s of UART output at 115200 baud
CLIENT_MAX_LINE = 1024
MAX_CLIENTS = 8
UART_MAX_LINE = 64 * 1024  # bytes without a newline that are logged as one line anyway
SECRET_MAX_S = 120.0  # a secret window ends this long after its newest secret command at the latest
# ... or this long after the last "fw: " line once the newest secret command's reply has begun. The UART console runs one
# command at a time and prints its reply in one go; a queued command waits at most 2 s for the console (RUN_WAIT_MS in
# fw_console/console.c) before it runs or says busy, so a reply never pauses this long in the middle.
SECRET_QUIET_S = 5.0
UNUSABLE = 3  # client exit code: fall back to the pause path
MASK = "***"


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


# Redaction is a denylist built from the console_register calls in firmware/components (a sent line not listed here is
# logged as typed): review it when a console command that takes or prints a secret is added. An allowlist like the
# console history's (lichen_shell/console_model.c) would fail closed; that is a later decision (#324).
#
# Commands whose arguments can carry a secret: a sent line that starts with these words (any case) keeps them and has
# the rest masked. "*" matches any one word (a name).
WIFI_SUBS = {"on", "off", "scan", "reconnect", "stats", "ps", "tx", "bw", "proto", "--clear"}
SECRET_ARGS = (
    (("wifi",), lambda rest: rest[0].lower() not in WIFI_SUBS),  # wifi <ssid> <password> (the SSID is private too)
    (("mesh", "remote", "*", "login"), None),  # mesh remote <who> login <password>
    (("mesh", "remote", "*", "cli"), None),  # cli <text>: "password x", "set guest.password x", "set prv.key x"
    (("channel", "add", "*"), None),  # channel add <name> <base64 key>; "channel add #name" has no key
)
# Name parts (split at anything but letters and digits) that make any other line secret: everything after its command
# word is masked. Whole parts, so bl.keyboard or "keys" are not a key.
SECRET_WORDS = {"password", "passwd", "pass", "pw", "secret", "key", "psk", "token"}
# Commands whose reply prints a secret on the UART (console_reply_secret goes to the asker in full, and the UART asker
# is everyone on the socket): they open a secret window (Hub.open_secret). Another client's line never closes it: the
# badge may still be printing the secret when that line is sent. The firmware prints a remote's answer (it comes over
# LoRa seconds later) only when "mesh remote" is run again, synchronously, but "mesh remote <who> login|cli" open a
# window too in case that changes. A core dump's body is always kept out (RAM).
SECRET_REPLIES = (("token",), ("channel", "show"), ("channel", "add"), ("mesh", "remote"))


def _split_fw(line: str) -> tuple[str, list[str]]:
    words = line.split()
    if words and words[0].lower() == "fw":
        return words[0] + " ", words[1:]
    return "", words


def _starts_with(words: list[str], pattern: tuple[str, ...]) -> bool:
    return len(words) >= len(pattern) and all(p in ("*", w.lower()) for p, w in zip(pattern, words))


def _secret_word(word: str) -> bool:
    return any(part in SECRET_WORDS for part in re.split(r"[^a-z0-9]+", word.lower()))


def redact(line: str) -> str:
    """The line as the log shows it: command words kept, arguments that can be a secret masked."""
    fw, words = _split_fw(line)
    for pattern, applies in SECRET_ARGS:
        rest = words[len(pattern) :]
        if _starts_with(words, pattern) and rest and (applies is None or applies(rest)):
            return fw + " ".join(words[: len(pattern)] + [MASK])
    if _starts_with(words, ("keys", "send")):  # text: types into the focused field, which may be a password field
        return fw + " ".join("text:" + MASK if w.lower().startswith("text:") else w for w in words)
    if len(words) > 1 and any(_secret_word(w) for w in words):
        return fw + " ".join(words[:1] + [MASK])
    return line


def reply_is_secret(line: str) -> bool:
    """Whether the reply to this sent line can print a secret: listed commands, and get/set of a secret-named key."""
    _, words = _split_fw(line)
    if any(_starts_with(words, pattern) for pattern in SECRET_REPLIES):
        return True
    return len(words) > 1 and words[0].lower() in ("get", "set") and _secret_word(words[1])


REFUSED_CONTROL = "--- refused: control characters (only tab is allowed), line not sent"
REFUSED_LONG = "--- refused: line longer than %d bytes, not sent" % CLIENT_MAX_LINE
# Refusals of the line itself: the pause path would refuse it too, so the client does not fall back to it
LINE_REFUSALS = (REFUSED_CONTROL, REFUSED_LONG)


def has_control(line: str) -> bool:
    """Whether a client line holds a C0 control character other than tab, or DEL.

    Refused, never stripped (#324): "se\x01nd" passes badge.sh's on-air gate as a word that is not "send" and would
    reach the badge as "send" if the logger cleaned it."""
    return any(ch != "\t" and (ch < " " or ch == "\x7f") for ch in line)


def socket_usable(sock_exists: bool, greeting: str, want_node: str) -> tuple[bool, str]:
    """Whether a console client may use the logger's socket for want_node (the realpath of the asked port).

    False sends badge.sh down the pause path of today: no logger, an old logger, the port paused or lost, or the
    logger holding another device than the one asked for."""
    if not sock_exists:
        return False, "no logger socket"
    words = greeting.split()
    if words[:2] != ["---", "connected"] or len(words) < 4:
        return False, "no greeting from the logger"
    if words[2] != "open":
        return False, "the logger's port is %s" % words[2]
    if words[3] != want_node:
        return False, "the logger has %s open, not %s" % (words[3], want_node)
    return True, "logger socket"


class Client:
    def __init__(self, sock: socket.socket) -> None:
        sock.setblocking(False)
        self.sock = sock
        self.inbuf = b""
        self.outbuf = bytearray()


class Hub:
    """The clients of the console socket: fan-out, bounded non-blocking sends, client lines to the UART.

    log(text) writes one log line, write_uart(bytes) sends to the badge, status(text) rewrites the status file."""

    def __init__(self, log, write_uart, status=None, max_buf: int = CLIENT_MAX_BUF) -> None:
        self.log, self.write_uart, self.status = log, write_uart, status
        self.max_buf = max_buf
        self.clients: list[Client] = []
        self.state, self.node = "lost", "-"
        # The secret window (#324): open from a secret command until secret_over(); the windows of several secret
        # commands are one, which ends only when the newest one's reply is over (fail closed)
        self.secret_until = 0.0  # its hard end, SECRET_MAX_S after the newest secret command; 0 = no window open
        self.secret_reply_at = 0.0  # the last "fw: " line since the newest secret command was sent; 0 = none yet
        self.withheld = 0
        self.dump_lines = -1  # lines of the core dump body kept out of the log; -1 outside a dump

    def write_status(self) -> None:
        if self.status:
            self.status("clients %d\nport %s %s\n" % (len(self.clients), self.state, self.node))

    def set_state(self, state: str, node: str = "-") -> None:
        self.state, self.node = state, node
        self.write_status()

    def add(self, sock: socket.socket) -> Client | None:
        if len(self.clients) >= MAX_CLIENTS:
            try:
                sock.setblocking(False)
                sock.send(b"--- refused: too many console clients\n")
            except OSError:
                pass
            sock.close()
            self.log("--- console client refused: %d connected already" % len(self.clients))
            return None
        c = Client(sock)
        self.clients.append(c)
        self.log("--- console client connected (%d)" % len(self.clients))
        self.queue(c, "--- connected %s %s" % (self.state, self.node))
        self.write_status()
        return c

    def drop(self, c: Client, why: str) -> None:
        if c not in self.clients:
            return
        self.clients.remove(c)
        try:
            c.sock.close()
        except OSError:
            pass
        self.log("--- console client %s (%d left)" % (why, len(self.clients)))
        self.write_status()

    def queue(self, c: Client, text: str) -> None:
        c.outbuf += (text + "\n").encode("utf-8", "replace")
        self.flush(c)

    def flush(self, c: Client) -> None:
        try:
            while c.outbuf:
                n = c.sock.send(c.outbuf)
                del c.outbuf[:n]
        except BlockingIOError:
            pass
        except OSError:
            self.drop(c, "gone")
            return
        if len(c.outbuf) > self.max_buf:
            self.drop(c, "dropped: it stopped reading")

    def flush_all(self) -> None:
        for c in list(self.clients):
            self.flush(c)

    def pending(self) -> bool:
        return any(c.outbuf for c in self.clients)

    def send_all(self, text: str, skip: Client | None = None) -> None:
        for c in list(self.clients):
            if c is not skip:
                self.queue(c, text)

    def note(self, text: str) -> None:
        """A "--- " line about the port: log and clients."""
        self.log(text)
        self.send_all(text)

    def keep_out_of_log(self, text: str, now: float) -> bool:
        """Whether a UART line goes to the clients only: a core dump's body, or a reply that can print a secret."""
        if text.startswith("--- coredump begin "):
            self.dump_lines = 0
            return False
        if self.dump_lines >= 0:
            if not text.startswith("--- coredump end "):
                self.dump_lines += 1
                return True
            self.log("--- %d core dump lines kept out of the log (RAM)" % self.dump_lines)
            self.dump_lines = -1
            return False
        self.tick(now)
        if self.secret_until and text.startswith("fw: "):
            self.withheld += 1
            self.secret_reply_at = now
            return True
        return False

    def uart_line(self, text: str, now: float) -> None:
        if not self.keep_out_of_log(text, now):
            self.log(text)
        self.send_all(text)

    def open_secret(self, now: float) -> None:
        """A command whose reply can print a secret was sent: its window opens, or the open one waits for it too."""
        self.secret_until = max(self.secret_until, now + SECRET_MAX_S)
        self.secret_reply_at = 0.0  # the reply of an older command never ends this one's window

    def secret_over(self, now: float) -> bool:
        """Whether the open window may close: its hard end, or the newest secret command's reply has been quiet."""
        if not self.secret_until:
            return False
        quiet = self.secret_reply_at and now - self.secret_reply_at >= SECRET_QUIET_S
        return now >= self.secret_until or bool(quiet)

    def end_withhold(self) -> None:
        self.log("--- %d reply lines kept out of the log (secret)" % self.withheld)
        self.secret_until = self.secret_reply_at = 0.0
        self.withheld = 0

    def tick(self, now: float) -> None:
        if self.secret_over(now):
            self.end_withhold()

    def readable(self, c: Client, now: float) -> None:
        try:
            data = c.sock.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            self.drop(c, "disconnected")  # half a line it left is not sent
            return
        c.inbuf += data
        while b"\n" in c.inbuf and c in self.clients:
            raw, c.inbuf = c.inbuf.split(b"\n", 1)
            if len(raw) > CLIENT_MAX_LINE:
                self.queue(c, REFUSED_LONG)
                continue
            line = raw.decode("utf-8", "replace")
            line = line[:-1] if line.endswith("\r") else line  # a CRLF client: the CR ends the line, as on the badge
            if has_control(line):
                self.log("--- console client line refused: control characters")  # not the line: it may be a secret
                self.queue(c, REFUSED_CONTROL)
                continue
            self.command(c, line, now)
        if len(c.inbuf) > CLIENT_MAX_LINE:
            self.drop(c, "dropped: line too long")

    def command(self, c: Client, line: str, now: float) -> None:
        """One client line: to the UART while the port is open, refused otherwise (never queued for later).

        A failed write is refused too and leaves the port open: only a failed read means it is gone, and every reopen
        of the CH340 risks a reset."""
        if self.state != "open":
            self.queue(c, "--- refused: the port is %s, line not sent" % self.state)
            return
        try:
            self.write_uart((line + "\n").encode("utf-8"))
        except Exception as e:  # noqa: BLE001 - pyserial raises its own timeout and I/O types
            self.log("--- write to the port failed (%s), line not sent" % e)
            self.queue(c, "--- refused: write failed (%s)" % e)
            return
        shown = ">> " + redact(line)  # an open secret window stays open: the badge may still print its reply
        self.log(shown)
        self.send_all(shown, skip=c)
        if reply_is_secret(line):
            self.open_secret(now)


def listen(path: str) -> socket.socket:
    """The console socket, mode 0600. A stale one (from a killed logger) is replaced; a live one is not taken over."""
    if os.path.lexists(path):
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            probe.connect(path)
            live = True
        except OSError:
            live = False
        finally:
            probe.close()
        if live:
            raise RuntimeError("another logger serves %s" % path)
        os.remove(path)
    lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    old = os.umask(0o177)
    try:
        lsock.bind(path)
    finally:
        os.umask(old)
    os.chmod(path, 0o600)
    lsock.listen(8)
    lsock.setblocking(False)
    return lsock


class Logger:
    """The port, the log file and the socket, in one thread: serve() waits on all of them at once."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.pause_path, self.paused_path = path + ".pause", path + ".paused"
        self.sock_path = os.path.join(os.path.dirname(path), SOCK_NAME)
        self.status_path = os.path.join(os.path.dirname(path), STATUS_NAME)
        self.lsock = listen(self.sock_path)  # first: a second logger stops here, before it touches anything
        self.out = open(path, "a", buffering=1)
        self.port = None
        self.lost_at = None  # wall time of the first failure of the current outage
        self.hub = Hub(self.log, self.write_uart, self.write_status)
        self.hub.write_status()

    def log(self, text: str) -> None:
        self.out.write(stamp() + text + "\n")

    def write_uart(self, data: bytes) -> None:
        if self.port is None:
            raise OSError("port not open")
        self.port.write(data)

    def write_status(self, text: str) -> None:
        tmp = self.status_path + ".tmp"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, self.status_path)

    def rotate(self) -> None:
        """Past MAX: the file moves to .1 and a new one is open before the old one closes, so no line is lost."""
        if self.out.tell() <= MAX:
            return
        os.replace(self.path, self.path + ".1")
        old, self.out = self.out, open(self.path, "a", buffering=1)
        old.close()

    def pause_requested(self) -> bool:
        try:
            return time.time() - os.path.getmtime(self.pause_path) < PAUSE_MAX_S
        except OSError:
            return False

    def detach(self) -> None:
        if self.port is not None:
            try:
                self.port.close()
            except Exception:  # noqa: BLE001 - closing a port that is gone
                pass
            self.port = None

    def serve(self, timeout: float) -> bytes:
        """One wait on the socket, the clients and the port; returns what the port gave (a port error raises)."""
        got = b""
        if self.hub.pending():
            timeout = min(timeout, 0.05)
        clients = {c.sock: c for c in self.hub.clients}
        waits = [self.lsock] + list(clients) + ([self.port] if self.port is not None else [])
        ready, _, _ = select.select(waits, [], [], timeout)
        now = time.time()
        for r in ready:
            if r is self.port:
                got += self.port.read(self.port.in_waiting or 1)
            else:
                self.guarded(self.accept if r is self.lsock else lambda: self.hub.readable(clients[r], now))
        self.guarded(self.hub.flush_all)
        self.hub.tick(now)
        return got

    def guarded(self, fn) -> None:
        """Client handling never stops the logger and never counts as a lost port: log it and go on."""
        try:
            fn()
        except Exception as e:  # noqa: BLE001 - the logger must outlive any client
            self.log("--- console client handling failed (%r), going on" % e)

    def accept(self) -> None:
        try:
            conn, _ = self.lsock.accept()
        except OSError:  # BlockingIOError, or out of file descriptors: the client retries or falls back
            return
        self.hub.add(conn)

    def wait(self, seconds: float) -> None:
        end = time.time() + seconds
        while time.time() < end:
            self.serve(max(0.0, min(0.1, end - time.time())))

    def wait_while_paused(self) -> None:
        open(self.paused_path, "w").close()
        self.hub.set_state("paused")
        self.hub.note("--- paused: port released for flashing")
        while self.pause_requested():
            self.serve(0.1)
        try:
            os.remove(self.paused_path)
        except OSError:
            pass
        self.hub.set_state("lost")
        self.hub.note("--- resumed")

    def port_open(self, s, port: str, dev: str, how: str) -> None:
        self.port = s
        node = os.path.realpath(dev)
        self.hub.set_state("open", node)
        if self.lost_at is None:
            self.hub.note("--- port open %s: %s (%s)" % (port, node, how))
        else:
            self.hub.note(outage_text(self.lost_at, time.time(), node, how))
            self.lost_at = None

    def port_lost(self, e: Exception) -> None:
        """The port failed to open or to read: clients stay connected and are told; their lines are refused."""
        self.detach()
        self.hub.set_state("lost")
        if self.lost_at is None:  # one line per outage, not one per retry
            self.lost_at = time.time()
            self.hub.note("--- port lost (%s), retrying" % e)

    def close(self) -> None:
        for path in (self.sock_path, self.status_path, self.paused_path):
            try:
                os.remove(path)
            except OSError:
                pass


def open_port(dev):
    import serial  # only on the flash host; the tests import this file without pyserial

    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = dev, 115200, 0, 2
    s.dsrdtr = True
    s.rts = False
    s.open()
    s.dtr = False
    return s


def split_uart(buf: bytes) -> tuple[list[str], bytes]:
    """Complete lines out of the UART buffer; a run of UART_MAX_LINE bytes without a newline counts as a line."""
    lines = []
    while b"\n" in buf or len(buf) >= UART_MAX_LINE:
        if b"\n" in buf and buf.index(b"\n") < UART_MAX_LINE:
            line, buf = buf.split(b"\n", 1)
        else:
            line, buf = buf[:UART_MAX_LINE], buf[UART_MAX_LINE:]
        lines.append(line.rstrip(b"\r").decode("utf-8", "replace"))
    return lines, buf


def run_port(lg: Logger) -> None:
    """Read the open port until a pause is asked for; a lost port raises."""
    buf = b""
    try:
        while not lg.pause_requested():
            lines, buf = split_uart(buf + lg.serve(0.2))
            for line in lines:
                lg.hub.uart_line(line, time.time())
            lg.rotate()
    finally:
        if buf:
            lg.hub.uart_line(buf.rstrip(b"\r").decode("utf-8", "replace"), time.time())


def logger_main(port: str, path: str) -> None:
    import serial

    os.makedirs(os.path.dirname(path), exist_ok=True)  # outside tmpfs: survives a reboot of the host (#276)
    lg = Logger(path)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # systemctl stop: the finally below removes the socket
    try:
        os.remove(lg.paused_path)  # stale marker from a previous run
    except OSError:
        pass
    try:
        while True:
            if lg.pause_requested():
                lg.wait_while_paused()
            dev, how = pick_port(port, os.path.exists(port), tty_usb_nodes())
            try:
                if dev is None:
                    raise OSError(how)
                lg.port_open(open_port(dev), port, dev, how)
                run_port(lg)
                lg.detach()
            except (serial.SerialException, OSError) as e:
                lg.port_lost(e)
                lg.wait(RETRY_S)
    finally:
        lg.close()


# --- the client side (badge.sh runs it on the flash host) ---


def sock_path_for(log: str = DEFAULT_LOG) -> str:
    return os.path.join(os.path.dirname(os.path.expanduser(log)), SOCK_NAME)


def read_greeting(s: socket.socket) -> str:
    """The logger's first line, byte by byte so nothing after it is read ahead and lost."""
    line = b""
    while not line.endswith(b"\n") and len(line) < 256:
        c = s.recv(1)
        if not c:
            break
        line += c
    return line.decode("utf-8", "replace")


def connect(port: str, sock_path: str) -> socket.socket | None:
    """A connection to the logger that may be used for port, or None (the reason on stderr)."""
    exists = os.path.exists(sock_path)
    greeting, s = "", None
    if exists:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(2)
            s.connect(sock_path)
            greeting = read_greeting(s)
        except OSError:
            greeting = ""
    ok, why = socket_usable(exists, greeting, os.path.realpath(port))
    if not ok:
        if s:
            s.close()
        sys.stderr.write("console: %s, pausing the logger instead\n" % why)
        return None
    s.settimeout(None)
    return s


def is_note(line: bytes) -> bool:
    """A line from the logger itself (a "--- " note or another client's ">> " line), not from the badge."""
    return line.startswith(b"--- ") or line.startswith(b">> ")


def read_lines(s: socket.socket, end: float, quiet: float | None, out, until: bytes = b"") -> str:
    """Copy lines from s to out until end, quiet seconds without data, or a line starting with until.

    The logger's refusal of the line ("--- refused: ...") or "" when it was not refused: a refusal can follow other
    notes, so every note before the badge's first line is checked."""
    buf, last, answered = b"", time.time(), False
    while True:
        now = time.time()
        limit = end if quiet is None else min(end, last + quiet)
        if now >= limit:
            return ""
        ready, _, _ = select.select([s], [], [], limit - now)
        if not ready:
            continue
        data = s.recv(4096)
        if not data:
            return ""
        last, buf = time.time(), buf + data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if not answered and line.startswith(b"--- refused"):
                out.flush()
                return line.decode("utf-8", "replace")
            answered = answered or not is_note(line)
            out.write(line.decode("utf-8", "replace") + "\n")
            if until and line.startswith(until):
                out.flush()
                return ""
        out.flush()


def client_send(
    port: str, line: str, seconds: float, quiet: float | None, sock_path: str, until: str = ""
) -> int:
    if has_control(line):  # before anything is sent; the pause path would refuse it too (badge.sh)
        sys.stderr.write("console: %s\n" % REFUSED_CONTROL[4:])
        return 1
    s = connect(port, sock_path)
    if s is None:
        return UNUSABLE
    with s:
        s.sendall((line + "\n").encode("utf-8"))
        refused = read_lines(s, time.time() + seconds, quiet, sys.stdout, until.encode())
    if refused.startswith(LINE_REFUSALS):  # the line itself: the pause path must not send it either
        sys.stderr.write("console: %s\n" % refused[4:])
        return 1
    return UNUSABLE if refused else 0


def client_monitor(port: str, sock_path: str) -> int:
    """stdin to the socket, the socket to stdout, until EOF (Ctrl-D) or Ctrl-C; the tty's line editing stays."""
    s = connect(port, sock_path)
    if s is None:
        return UNUSABLE
    print("--- console through the serial logger: one line per Enter, Ctrl-D or Ctrl-C leaves", flush=True)
    try:
        while True:
            ready, _, _ = select.select([s, 0], [], [])
            if 0 in ready:
                data = os.read(0, 4096)
                if not data:
                    return 0
                s.sendall(data)
            if s in ready:
                data = s.recv(4096)
                if not data:
                    print("--- the logger closed the connection")
                    return 1
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    except KeyboardInterrupt:
        return 0


def client_main(argv: list[str]) -> int:
    """--send <port> <line> [s] | --session <port> <line> <quiet_s> <max_s> [until] | --monitor <port>.

    A leading --b64 means <line> is base64 (badge.sh passes it so through the remote shell: any quote works)."""
    sock_path = os.environ.get("BADGE_CONSOLE_SOCK") or sock_path_for()
    b64 = argv[:1] == ["--b64"]
    argv = argv[1:] if b64 else argv
    mode, args = (argv[0], argv[1:]) if argv else ("", [])
    if b64 and len(args) >= 2:
        args[1] = base64.b64decode(args[1]).decode("utf-8")
    if mode == "--send" and len(args) in (2, 3):
        return client_send(args[0], args[1], float(args[2]) if len(args) == 3 else 2.0, None, sock_path)
    if mode == "--session" and len(args) in (4, 5):
        until = args[4] if len(args) == 5 else ""
        return client_send(args[0], args[1], float(args[3]), float(args[2]), sock_path, until)
    if mode == "--monitor" and len(args) == 1:
        return client_monitor(args[0], sock_path)
    sys.stderr.write("usage: serial_log.py [--b64] --send|--session|--monitor ... (see the head of the file)\n")
    return 2


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1].startswith("--"):
        sys.exit(client_main(sys.argv[1:]))
    port = sys.argv[1] if len(sys.argv) > 1 else BY_ID
    path = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(DEFAULT_LOG)
    logger_main(port, path)


if __name__ == "__main__":
    main()
