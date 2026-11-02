# tools/serial_log.py port choice (#115): by-id while it exists, else the single CH340 node, never another adapter.
import importlib.util
import pathlib

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


if __name__ == "__main__":  # ctest runs this with the build's python, which may lack pytest
    tests = [f for name, f in sorted(globals().items()) if name.startswith("test_") and callable(f)]
    for test in tests:
        test()
    print("%d passed" % len(tests))
