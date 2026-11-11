# tools/serial_log.py: port choice (#115): by-id while it exists, else the single CH340 node, never another adapter;
# the console socket (#324): redaction, framing and fan-out, a stalled client dropped, refusal while paused, and the
# client's choice between the socket and the pause path.
import base64
import importlib.util
import io
import os
import pathlib
import socket
import stat
import tempfile
import threading
import time

_SPEC = importlib.util.spec_from_file_location(
    "serial_log", pathlib.Path(__file__).resolve().parent.parent / "tools" / "serial_log.py"
)
assert _SPEC is not None and _SPEC.loader is not None
serial_log = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(serial_log)

BY_ID = serial_log.BY_ID
CP2102 = ("/dev/ttyUSB0", "cp210x")


def test_by_id_wins_while_it_exists() -> None:
    assert serial_log.pick_port(BY_ID, True, [CP2102, ("/dev/ttyUSB2", "ch341-uart")]) == (BY_ID, "by-id")


def test_falls_back_to_the_ch340_node_when_by_id_is_missing() -> None:
    nodes = [CP2102, ("/dev/ttyUSB2", "ch341-uart")]
    assert serial_log.pick_port(BY_ID, False, nodes) == ("/dev/ttyUSB2", "fallback")


def test_never_opens_another_adapter() -> None:
    dev, why = serial_log.pick_port(BY_ID, False, [CP2102])
    assert dev is None and "no CH340" in why


def test_two_ch340_nodes_are_not_guessed() -> None:
    nodes = [("/dev/ttyUSB1", "ch341-uart"), ("/dev/ttyUSB2", "ch341-uart")]
    dev, why = serial_log.pick_port(BY_ID, False, nodes)
    assert dev is None and "2 CH340" in why


def test_outage_line_names_the_gap_and_node() -> None:
    line = serial_log.outage_text(100.0, 163.4, "/dev/ttyUSB2", "fallback")
    assert line == "--- port back after 63 s: /dev/ttyUSB2 (fallback)"


# --- console socket (#324) ---

SECRET_LINES = [
    ("fw wifi MyNet hunter22", "fw wifi ***"),
    ("wifi MyNet hunter22", "wifi ***"),
    ("fw mesh remote rpt1 login hunter22", "fw mesh remote rpt1 login ***"),
    ("fw mesh remote a1b2c3 cli password hunter22", "fw mesh remote a1b2c3 cli ***"),
    ("fw mesh remote a1b2c3 cli set guest.password x", "fw mesh remote a1b2c3 cli ***"),
    ("fw channel add Team 8b3387e9c5cdea6ac9e5edbaa115cd72", "fw channel add Team ***"),
    ("fw keys send down text:hunter22 enter", "fw keys send down text:*** enter"),
    ("fw set some.password hunter22", "fw set ***"),
    ("fw qr my secret", "fw qr ***"),
]

KEPT_LINES = [
    "fw get bl.",
    "fw wifi scan",
    "fw wifi tx 40",
    "fw wifi --clear",
    "fw wifi",
    "fw channel add #badgetest",
    "fw mesh remote rpt1 login",
    "fw mesh remote rpt1 status",
    "fw keys send down enter",
    "fw token",
]


def test_each_secret_command_is_masked() -> None:
    for line, shown in SECRET_LINES:
        assert serial_log.redact(line) == shown, line


def test_normal_commands_are_kept() -> None:
    for line in KEPT_LINES:
        assert serial_log.redact(line) == line, line


def test_replies_that_print_a_secret() -> None:
    for line in ["fw token", "token", "fw channel show 1", "fw mesh remote rpt1 cli get prv.key"]:
        assert serial_log.reply_is_secret(line), line
    for line in ["fw get bl.", "fw crash", "fw crash dump", "fw channels", "fw mesh"]:  # a dump: its own rule
        assert not serial_log.reply_is_secret(line), line


def test_socket_used_only_when_the_logger_has_the_port_open() -> None:
    use = serial_log.socket_usable
    assert use(True, "--- connected open /dev/ttyUSB1\n", "/dev/ttyUSB1") == (True, "logger socket")
    assert not use(False, "", "/dev/ttyUSB1")[0]  # no logger (or an old one without the socket)
    assert not use(True, "", "/dev/ttyUSB1")[0]  # the socket is there but nobody answers
    assert not use(True, "--- connected paused -\n", "/dev/ttyUSB1")[0]  # flashing
    assert not use(True, "--- connected lost -\n", "/dev/ttyUSB1")[0]
    assert "not /dev/ttyACM0" in use(True, "--- connected open /dev/ttyUSB1\n", "/dev/ttyACM0")[1]


class FakeLogger:
    def __init__(self, state="open"):
        self.lines, self.uart = [], []
        self.hub = serial_log.Hub(self.lines.append, self.uart.append)
        self.hub.set_state(state, "/dev/ttyUSB1")

    def client(self):
        ours, theirs = socket.socketpair()
        theirs.settimeout(1)
        return self.hub.add(ours), theirs


def received(peer: socket.socket) -> list[str]:
    peer.settimeout(0.2)
    data = b""
    try:
        while True:
            chunk = peer.recv(65536)
            if not chunk:
                break
            data += chunk
    except (socket.timeout, BlockingIOError):
        pass
    return data.decode().splitlines()


def test_lines_are_framed_and_fanned_out_to_every_client() -> None:
    lg = FakeLogger()
    a, peer_a = lg.client()
    _, peer_b = lg.client()
    assert received(peer_a) == ["--- connected open /dev/ttyUSB1"]
    received(peer_b)
    peer_a.sendall(b"fw get b")
    lg.hub.readable(a, 0.0)
    assert lg.uart == []  # half a line waits for its newline
    peer_a.sendall(b"l.\r\nfw mem\n")
    lg.hub.readable(a, 0.0)
    assert lg.uart == [b"fw get bl.\n", b"fw mem\n"]
    lg.hub.uart_line("fw: bl.display 40", 0.0)
    assert lg.lines[-3:] == [">> fw get bl.", ">> fw mem", "fw: bl.display 40"]
    assert received(peer_a) == ["fw: bl.display 40"]  # the sender does not get its own line back
    assert received(peer_b) == [">> fw get bl.", ">> fw mem", "fw: bl.display 40"]


def test_a_secret_is_masked_in_the_log_and_for_other_clients() -> None:
    lg = FakeLogger()
    a, peer_a = lg.client()
    _, peer_b = lg.client()
    peer_a.sendall(b"fw wifi MyNet hunter22\n")
    lg.hub.readable(a, 0.0)
    assert lg.uart == [b"fw wifi MyNet hunter22\n"]  # the badge gets the real line
    assert ">> fw wifi ***" in lg.lines and not any("hunter22" in l for l in lg.lines)
    assert not any("hunter22" in l for l in received(peer_b))


def test_a_secret_reply_reaches_the_clients_but_not_the_log() -> None:
    lg = FakeLogger()
    a, peer_a = lg.client()
    peer_a.sendall(b"fw token\n")
    lg.hub.readable(a, 100.0)
    lg.hub.uart_line("fw: token 0123456789abcdef0123456789abcdef", 100.5)
    lg.hub.uart_line("I (1) a log line is kept", 101.0)
    assert "fw: token 0123456789abcdef0123456789abcdef" in received(peer_a)
    peer_a.sendall(b"fw mem\n")  # the next sent line ends the window
    lg.hub.readable(a, 102.0)
    lg.hub.uart_line("fw: heap", 102.5)
    assert not any("0123456789abcdef" in l for l in lg.lines)
    assert lg.lines[-4:] == ["I (1) a log line is kept", "--- 1 reply lines kept out of the log (secret)",
                             ">> fw mem", "fw: heap"]


def test_a_remote_reply_over_lora_seconds_later_is_still_kept_out() -> None:
    lg = FakeLogger()
    a, peer_a = lg.client()
    peer_a.sendall(b"fw mesh remote rpt1 cli get guest.password\n")
    lg.hub.readable(a, 100.0)
    lg.hub.tick(130.0)
    lg.hub.uart_line("fw:   guest.password hunter22", 130.0)  # the repeater answers 30 s later
    assert not any("hunter22" in l for l in lg.lines)
    lg.hub.tick(100.0 + serial_log.SECRET_MAX_S)
    lg.hub.uart_line("fw: late", 100.0 + serial_log.SECRET_MAX_S + 1)
    assert lg.lines[-2:] == ["--- 1 reply lines kept out of the log (secret)", "fw: late"]


def test_a_core_dump_body_stays_out_of_the_log() -> None:
    lg = FakeLogger()
    _, peer = lg.client()
    for text in ["fw: crash: elf ab12", "--- coredump begin 6 ---", "AAAAAAAA", "", "--- coredump end 1234abcd ---"]:
        lg.hub.uart_line(text, 0.0)
    assert "AAAAAAAA" in received(peer)
    assert "AAAAAAAA" not in lg.lines
    assert lg.lines[-3:] == ["--- coredump begin 6 ---", "--- 2 core dump lines kept out of the log (RAM)",
                             "--- coredump end 1234abcd ---"]


def test_get_and_set_of_a_secret_named_key_keep_their_reply_out() -> None:
    assert serial_log.reply_is_secret("fw get wifi.pass")
    assert serial_log.reply_is_secret("FW SET mesh.prv_key 00")
    assert not serial_log.reply_is_secret("fw get bl.keyboard")
    assert not serial_log.reply_is_secret("fw get bl.")


def test_redaction_ignores_case_and_matches_whole_name_parts() -> None:
    assert serial_log.redact("FW WiFi MyNet hunter22") == "FW WiFi ***"
    assert serial_log.redact("fw token 0123") == "fw token ***"
    assert serial_log.redact("fw set wifi.pass hunter22") == "fw set ***"
    assert serial_log.redact("fw set mesh.prv.key 00") == "fw set ***"
    assert serial_log.redact("fw set bl.keyboard 40") == "fw set bl.keyboard 40"
    assert serial_log.redact("fw keys on") == "fw keys on"


def test_a_failed_write_is_refused_and_keeps_the_port() -> None:
    lg = FakeLogger()

    def timeout(_data):
        raise RuntimeError("Write timeout")  # pyserial's SerialTimeoutException is no OSError

    lg.hub.write_uart = timeout
    a, peer = lg.client()
    received(peer)
    peer.sendall(b"fw mem\n")
    lg.hub.readable(a, 0.0)  # must not raise: a raise would reopen the CH340
    assert received(peer) == ["--- refused: write failed (Write timeout)"]
    assert lg.hub.state == "open" and a in lg.hub.clients


def test_control_characters_are_stripped_and_long_lines_refused() -> None:
    lg = FakeLogger()
    a, peer = lg.client()
    received(peer)
    peer.sendall(b"fw\x1b[2J m\x03em\tx\x7f\n" + b"x" * 1500 + b"\nfw boot\n")
    lg.hub.readable(a, 0.0)
    lg.hub.readable(a, 0.0)
    assert lg.uart == [b"fw[2J mem\tx\n", b"fw boot\n"]
    assert "--- refused: line longer than 1024 bytes, not sent" in received(peer)
    assert a in lg.hub.clients


def test_a_client_that_disconnects_mid_line_sends_nothing() -> None:
    lg = FakeLogger()
    a, peer = lg.client()
    peer.sendall(b"fw wifi MyNet hunt")
    lg.hub.readable(a, 0.0)
    peer.close()
    lg.hub.readable(a, 0.0)
    assert lg.uart == [] and a not in lg.hub.clients
    assert lg.lines[-1] == "--- console client disconnected (0 left)"


def test_clients_are_capped() -> None:
    lg = FakeLogger()
    peers = [lg.client()[1] for _ in range(serial_log.MAX_CLIENTS)]
    ours, theirs = socket.socketpair()
    assert lg.hub.add(ours) is None
    theirs.settimeout(1)
    assert theirs.recv(100) == b"--- refused: too many console clients\n"
    assert len(lg.hub.clients) == serial_log.MAX_CLIENTS and len(peers) == serial_log.MAX_CLIENTS


def test_a_note_before_the_refusal_still_means_refused() -> None:
    ours, theirs = socket.socketpair()
    theirs.sendall(b"--- port lost (gone), retrying\n--- refused: the port is lost, line not sent\n")
    out = io.StringIO()
    assert serial_log.read_lines(ours, time.time() + 1, None, out) is False
    ours, theirs = socket.socketpair()
    theirs.sendall(b"fw: answer\n--- refused: someone else\n")  # after the badge answered: not about our line
    assert serial_log.read_lines(ours, time.time() + 0.3, None, io.StringIO()) is True


def test_a_session_ends_at_its_marker() -> None:
    ours, theirs = socket.socketpair()
    theirs.sendall(b"fw: crash: elf ab\n--- coredump begin 3 ---\nAAAA\n--- coredump end 01 ---\n")
    out = io.StringIO()
    start = time.time()
    assert serial_log.read_lines(ours, start + 30, 10, out, b"--- coredump end ") is True
    assert time.time() - start < 2
    assert out.getvalue().endswith("--- coredump end 01 ---\n")


def test_a_uart_run_without_newline_is_cut() -> None:
    lines, rest = serial_log.split_uart(b"a\r\n" + b"x" * (serial_log.UART_MAX_LINE + 5))
    assert lines == ["a", "x" * serial_log.UART_MAX_LINE] and rest == b"xxxxx"


def test_a_live_socket_is_not_taken_over() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, serial_log.SOCK_NAME)
        first = serial_log.listen(path)
        try:
            try:
                serial_log.listen(path)
                raise AssertionError("a second logger took the socket")
            except RuntimeError as e:
                assert "another logger" in str(e)
        finally:
            first.close()


class FakePort:
    def __init__(self):
        self.written = []

    def write(self, data):
        self.written.append(data)

    def close(self):
        pass


def connect_client(lg) -> socket.socket:
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    peer.connect(lg.sock_path)
    lg.accept()
    return peer


def test_port_loss_keeps_clients_and_refuses_their_lines() -> None:
    with tempfile.TemporaryDirectory() as d:
        lg = serial_log.Logger(os.path.join(d, "badge-serial.log"))
        try:
            lg.port_open(FakePort(), "/dev/badge-console", "/dev/null", "by-id")
            peer = connect_client(lg)
            assert received(peer) == ["--- connected open /dev/null"]
            lg.port_lost(OSError("device disconnected"))
            peer.sendall(b"fw mem\n")
            lg.hub.readable(lg.hub.clients[0], 0.0)
            assert received(peer) == ["--- port lost (device disconnected), retrying",
                                      "--- refused: the port is lost, line not sent"]
            assert len(lg.hub.clients) == 1
            assert open(lg.status_path).read() == "clients 1\nport lost -\n"
        finally:
            lg.close()


def test_pause_entry_tells_the_clients_and_refuses_while_paused() -> None:
    with tempfile.TemporaryDirectory() as d:
        lg = serial_log.Logger(os.path.join(d, "badge-serial.log"))
        try:
            peer = connect_client(lg)
            received(peer)
            open(lg.pause_path, "w").close()
            done = threading.Thread(target=lg.wait_while_paused)
            done.start()
            time.sleep(0.3)
            peer.sendall(b"fw mem\n")
            time.sleep(0.3)
            assert os.path.exists(lg.paused_path)
            os.remove(lg.pause_path)
            done.join(5)
            assert received(peer) == ["--- paused: port released for flashing",
                                      "--- refused: the port is paused, line not sent", "--- resumed"]
        finally:
            lg.close()


def test_rotation_opens_the_new_file_first() -> None:
    with tempfile.TemporaryDirectory() as d:
        lg = serial_log.Logger(os.path.join(d, "badge-serial.log"))
        saved, serial_log.MAX = serial_log.MAX, 10
        try:
            lg.log("a line longer than ten bytes")
            lg.rotate()
            lg.log("next")
            assert "next" in open(lg.path).read() and "longer" in open(lg.path + ".1").read()
        finally:
            serial_log.MAX = saved
            lg.close()


def test_a_stalled_client_is_dropped_without_blocking() -> None:
    lg = FakeLogger()
    lg.hub.max_buf = 64 * 1024
    stalled, _peer_stalled = lg.client()  # never reads
    _, peer_ok = lg.client()
    received(peer_ok)
    start = time.time()
    line = "x" * 1000
    for _ in range(2000):  # 2 MB: far more than socket buffers plus max_buf
        lg.hub.uart_line(line, 0.0)
        received_nowait(peer_ok)  # the other client keeps up
    assert time.time() - start < 5
    assert stalled not in lg.hub.clients and len(lg.hub.clients) == 1
    assert any("dropped: it stopped reading" in l for l in lg.lines)


def received_nowait(peer: socket.socket) -> bytes:
    peer.setblocking(False)
    data = b""
    try:
        while True:
            chunk = peer.recv(1 << 20)
            if not chunk:
                break
            data += chunk
    except BlockingIOError:
        pass
    return data


def test_while_paused_a_line_is_refused_not_queued() -> None:
    lg = FakeLogger("paused")
    a, peer = lg.client()
    assert received(peer) == ["--- connected paused /dev/ttyUSB1"]
    peer.sendall(b"fw get bl.\n")
    lg.hub.readable(a, 0.0)
    assert lg.uart == []
    assert received(peer) == ["--- refused: the port is paused, line not sent"]
    lg.hub.set_state("open", "/dev/ttyUSB1")
    lg.hub.readable(a, 0.0)  # nothing queued comes out later
    assert lg.uart == []


def test_socket_is_private_and_a_stale_one_is_replaced() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, serial_log.SOCK_NAME)
        open(path, "w").close()  # left over from a killed logger
        lsock = serial_log.listen(path)
        try:
            mode = os.stat(path).st_mode
            assert stat.S_ISSOCK(mode) and stat.S_IMODE(mode) == 0o600
        finally:
            lsock.close()


def test_client_without_a_logger_asks_for_the_pause_path() -> None:
    with tempfile.TemporaryDirectory() as d:
        missing = os.path.join(d, serial_log.SOCK_NAME)
        assert serial_log.client_send("/dev/null", "fw mem", 0.1, None, missing) == serial_log.UNUSABLE



def test_a_base64_line_arrives_with_its_quotes() -> None:
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, serial_log.SOCK_NAME)
        lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        lsock.bind(path)
        lsock.listen(1)
        got = []

        def logger():
            conn, _ = lsock.accept()
            conn.sendall(b"--- connected open /dev/null\n")
            got.append(conn.makefile("rb").readline())
            conn.close()

        t = threading.Thread(target=logger)
        t.start()
        line = "fw qr it's \"$HOME\" `x`"
        saved = os.environ.get("BADGE_CONSOLE_SOCK")
        os.environ["BADGE_CONSOLE_SOCK"] = path
        try:
            arg = base64.b64encode(line.encode()).decode()
            assert serial_log.client_main(["--b64", "--send", "/dev/null", arg, "0.2"]) == 0
        finally:
            if saved is None:
                del os.environ["BADGE_CONSOLE_SOCK"]
            else:
                os.environ["BADGE_CONSOLE_SOCK"] = saved
            t.join(5)
            lsock.close()
        assert got == [(line + "\n").encode()]

if __name__ == "__main__":  # ctest runs this with the build's python, which may lack pytest
    tests = [f for name, f in sorted(globals().items()) if name.startswith("test_") and callable(f)]
    for test in tests:
        test()
    print("%d passed" % len(tests))
