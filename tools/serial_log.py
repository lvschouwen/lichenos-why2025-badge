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
#     never queued;
#   - every line read from the UART and every "--- " note goes to the log and to every client; the reply to a
#     command that prints a secret (SECRET_REPLIES) goes to the clients only, the log gets a count;
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
#        serial_log.py --send <port> <line> [seconds] | --session <port> <line> <quiet_s> <max_s> | --monitor <port>
import glob
import os
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
SECRET_QUIET_S = 3.0  # a withheld reply ends after this long without a UART line ...
SECRET_MAX_S = 120.0  # ... or this long after the command (a core dump takes about a minute)
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


# Console commands whose arguments can carry a secret (from the console_register calls in firmware/components): a sent
# line that starts with these words keeps them and has the rest masked. "*" matches any one word (a name).
WIFI_SUBS = {"on", "off", "scan", "reconnect", "stats", "ps", "tx", "bw", "proto", "--clear"}
SECRET_ARGS = (
    (("wifi",), lambda rest: rest[0] not in WIFI_SUBS),  # wifi <ssid> <password> (the SSID is private too)
    (("mesh", "remote", "*", "login"), None),  # mesh remote <who> login <password>
    (("mesh", "remote", "*", "cli"), None),  # cli <text>: "password x", "set guest.password x", "set prv.key x"
    (("channel", "add", "*"), None),  # channel add <name> <base64 key>; "channel add #name" has no key
)
# Words that make any other line secret: everything after its command word is masked
SECRET_WORDS = ("password", "passwd", "secret", "prv.key", "psk")
# Commands whose reply prints a secret on the UART (console_reply_secret goes to the asker in full, and the UART asker
# is everyone on the socket): the reply is kept out of the log. crash dump: the core dump holds RAM.
SECRET_REPLIES = (("token",), ("channel", "show"), ("channel", "add"), ("mesh", "remote"), ("crash", "dump"))


def _split_fw(line: str) -> tuple[str, list[str]]:
    words = line.split()
    if words and words[0] == "fw":
        return "fw ", words[1:]
    return "", words


def _starts_with(words: list[str], pattern: tuple[str, ...]) -> bool:
    return len(words) >= len(pattern) and all(p in ("*", w) for p, w in zip(pattern, words))


def redact(line: str) -> str:
    """The line as the log shows it: command words kept, arguments that can be a secret masked."""
    fw, words = _split_fw(line)
    for pattern, applies in SECRET_ARGS:
        rest = words[len(pattern) :]
        if _starts_with(words, pattern) and rest and (applies is None or applies(rest)):
            return fw + " ".join(words[: len(pattern)] + [MASK])
    if words[:2] == ["keys", "send"]:  # text: types into the focused field, which may be a password field
        return fw + " ".join("text:" + MASK if w.startswith("text:") else w for w in words)
    if any(s in w.lower() for w in words[1:] for s in SECRET_WORDS):
        return fw + " ".join(words[:1] + [MASK])
    return line


def reply_is_secret(line: str) -> bool:
    _, words = _split_fw(line)
    return any(_starts_with(words, pattern) for pattern in SECRET_REPLIES)


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
        self.withhold_quiet = self.withhold_end = 0.0
        self.withheld = 0

    def write_status(self) -> None:
        if self.status:
            self.status("clients %d\nport %s %s\n" % (len(self.clients), self.state, self.node))

    def set_state(self, state: str, node: str = "-") -> None:
        self.state, self.node = state, node
        self.write_status()

    def add(self, sock: socket.socket) -> Client:
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

    def uart_line(self, text: str, now: float) -> None:
        if self.withhold_end:
            if now < self.withhold_quiet and now < self.withhold_end:
                self.withheld += 1
                self.withhold_quiet = now + SECRET_QUIET_S
                self.send_all(text)
                return
            self.end_withhold()
        self.log(text)
        self.send_all(text)

    def end_withhold(self) -> None:
        self.log("--- %d reply lines kept out of the log (secret)" % self.withheld)
        self.withhold_quiet = self.withhold_end = 0.0
        self.withheld = 0

    def tick(self, now: float) -> None:
        if self.withhold_end and (now >= self.withhold_quiet or now >= self.withhold_end):
            self.end_withhold()

    def readable(self, c: Client, now: float) -> None:
        try:
            data = c.sock.recv(4096)
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if not data:
            self.drop(c, "disconnected")
            return
        c.inbuf += data
        while b"\n" in c.inbuf and c in self.clients:
            raw, c.inbuf = c.inbuf.split(b"\n", 1)
            self.command(c, raw.rstrip(b"\r").decode("utf-8", "replace"), now)
        if len(c.inbuf) > CLIENT_MAX_LINE:
            self.drop(c, "dropped: line too long")

    def command(self, c: Client, line: str, now: float) -> None:
        """One client line: to the UART while the port is open, refused otherwise (never queued for later)."""
        if self.state != "open":
            self.queue(c, "--- refused: the port is %s, line not sent" % self.state)
            return
        self.write_uart((line + "\n").encode("utf-8"))
        shown = ">> " + redact(line)
        self.log(shown)
        self.send_all(shown, skip=c)
        if reply_is_secret(line):
            self.withhold_quiet, self.withhold_end = now + SECRET_QUIET_S, now + SECRET_MAX_S


def listen(path: str) -> socket.socket:
    """The console socket, mode 0600; a stale one from a killed logger is replaced."""
    try:
        os.remove(path)
    except FileNotFoundError:
        pass
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
        self.out = open(path, "a", buffering=1)
        self.port = None
        self.lsock = listen(self.sock_path)
        self.hub = Hub(self.log, self.write_uart, self.write_status)
        self.hub.write_status()

    def log(self, text: str) -> None:
        self.out.write(stamp() + text + "\n")

    def write_uart(self, data: bytes) -> None:
        self.port.write(data)

    def write_status(self, text: str) -> None:
        tmp = self.status_path + ".tmp"
        with open(tmp, "w") as f:
            f.write(text)
        os.replace(tmp, self.status_path)

    def pause_requested(self) -> bool:
        try:
            return time.time() - os.path.getmtime(self.pause_path) < PAUSE_MAX_S
        except OSError:
            return False

    def detach(self) -> None:
        if self.port is not None:
            try:
                self.port.close()
            except OSError:
                pass
            self.port = None

    def serve(self, timeout: float) -> bytes:
        """One wait on the socket, the clients and the port; returns what the port gave."""
        got = b""
        if self.hub.pending():
            timeout = min(timeout, 0.05)
        clients = {c.sock: c for c in self.hub.clients}
        waits = [self.lsock] + list(clients) + ([self.port] if self.port is not None else [])
        ready, _, _ = select.select(waits, [], [], timeout)
        now = time.time()
        for r in ready:
            if r is self.lsock:
                self.accept()
            elif r is self.port:
                got += self.port.read(self.port.in_waiting or 1)
            else:
                self.hub.readable(clients[r], now)
        self.hub.flush_all()
        self.hub.tick(now)
        return got

    def accept(self) -> None:
        try:
            conn, _ = self.lsock.accept()
        except BlockingIOError:
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


def run_port(lg: Logger, s) -> None:
    """Read the open port until a pause is asked for; a lost port raises."""
    buf = b""
    try:
        while not lg.pause_requested():
            buf += lg.serve(0.2)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                lg.hub.uart_line(line.rstrip(b"\r").decode("utf-8", "replace"), time.time())
            if lg.out.tell() > MAX:
                lg.out.close()
                os.replace(lg.path, lg.path + ".1")
                lg.out = open(lg.path, "a", buffering=1)
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
    lost_at = None  # wall time of the first failure of the current outage
    try:
        while True:
            if lg.pause_requested():
                lg.wait_while_paused()
            dev, how = pick_port(port, os.path.exists(port), tty_usb_nodes())
            try:
                if dev is None:
                    raise OSError(how)
                lg.port = open_port(dev)
                node = os.path.realpath(dev)
                lg.hub.set_state("open", node)
                if lost_at is None:
                    lg.hub.note("--- port open %s: %s (%s)" % (port, node, how))
                else:
                    lg.hub.note(outage_text(lost_at, time.time(), node, how))
                    lost_at = None
                run_port(lg, lg.port)
                lg.detach()
            except (serial.SerialException, OSError) as e:
                lg.detach()
                lg.hub.set_state("lost")
                if lost_at is None:  # one line per outage, not one per retry
                    lost_at = time.time()
                    lg.hub.note("--- port lost (%s), retrying" % e)
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


def read_lines(s: socket.socket, end: float, quiet: float | None, out) -> bool:
    """Copy lines from s to out until end, or quiet seconds without data; False when the line was refused."""
    buf, last, first = b"", time.time(), True
    while True:
        now = time.time()
        limit = end if quiet is None else min(end, last + quiet)
        if now >= limit:
            return True
        ready, _, _ = select.select([s], [], [], limit - now)
        if not ready:
            continue
        data = s.recv(4096)
        if not data:
            return True
        last, buf = time.time(), buf + data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if first and line.startswith(b"--- refused"):
                return False
            first = False
            out.write(line.decode("utf-8", "replace") + "\n")
        out.flush()


def client_send(port: str, line: str, seconds: float, quiet: float | None, sock_path: str) -> int:
    s = connect(port, sock_path)
    if s is None:
        return UNUSABLE
    with s:
        s.sendall((line + "\n").encode("utf-8"))
        ok = read_lines(s, time.time() + seconds, quiet, sys.stdout)
    return 0 if ok else UNUSABLE


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
    sock_path = os.environ.get("BADGE_CONSOLE_SOCK") or sock_path_for()
    mode, args = argv[0], argv[1:]
    if mode == "--send" and len(args) in (2, 3):
        return client_send(args[0], args[1], float(args[2]) if len(args) == 3 else 2.0, None, sock_path)
    if mode == "--session" and len(args) == 4:
        return client_send(args[0], args[1], float(args[3]), float(args[2]), sock_path)
    if mode == "--monitor" and len(args) == 1:
        return client_monitor(args[0], sock_path)
    sys.stderr.write(__doc__ or "usage: see the head of serial_log.py\n")
    return 2


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1].startswith("--"):
        sys.exit(client_main(sys.argv[1:]))
    port = sys.argv[1] if len(sys.argv) > 1 else BY_ID
    path = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser(DEFAULT_LOG)
    logger_main(port, path)


if __name__ == "__main__":
    main()
