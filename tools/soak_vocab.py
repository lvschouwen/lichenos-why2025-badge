#!/usr/bin/env python3
"""Vocabulary of the firmware's own log words (#430): `soak_vocab.py [--write | --check]`.
The soak census (tools/soak_census.py) prints a log line kind only from these words; every other token (a name, a
network, message text) is replaced. The words are the lowercase ASCII words of the string literals inside the
ESP_LOG*/LICHEN_LOG*/console_reply* calls and of the TAG definitions under firmware/main, firmware/components,
c6/main and shared (no submodules, no third_party, no build or managed_components directories). Format conversions
(%d, %-5s, %lu), escapes and 0x numbers are dropped first; one-letter words are dropped except `a`.
--write rewrites tools/soak_vocab.txt (sorted, one word per line); --check exits 1 when it is stale (a host test
does the same). Standard library only."""
import os
import re
import sys
from typing import Iterable, List, Set

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VOCAB_FILE = os.path.join(HERE, "soak_vocab.txt")
SCAN = ("firmware/main", "firmware/components", "c6/main", "shared")
SKIP_DIRS = {"build", "managed_components", "third_party", ".git", "node_modules"}
EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")
CALL = re.compile(r"\b(?:ESP_\w*LOG\w*|LICHEN_LOG\w*|console_reply\w*)\s*\(")
TAGDEF = re.compile(r'(?:\bTAG\w*|\w*[Tt]ag\w*)\s*(?:\[\s*\])?\s*=\s*"((?:[^"\\\n]|\\.)*)"|'
                    r'#\s*define\s+\w*TAG\w*\s+"((?:[^"\\\n]|\\.)*)"')
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESCAPE = re.compile(r"\\.")
FORMAT = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXfFeEgGcspn%]")
HEXNUM = re.compile(r"\b0[xX][0-9a-fA-F]+")
WORD = re.compile(r"[A-Za-z]+")


def call_literals(text: str, start: int) -> List[str]:
    """The string literals inside the parentheses that open at text[start - 1], in order."""
    depth, i, out = 1, start, []
    while i < len(text) and depth:
        c = text[i]
        if c == '"':
            m = LITERAL.match(text, i)
            if m is None:
                break
            out.append(m.group(1))
            i = m.end()
            continue
        depth += (c == "(") - (c == ")")
        i += 1
    return out


def words_of(literal: str) -> Set[str]:
    s = HEXNUM.sub(" ", FORMAT.sub(" ", ESCAPE.sub(" ", literal)))
    return {w.lower() for w in WORD.findall(s) if len(w) > 1 or w.lower() == "a"}


def file_words(text: str) -> Set[str]:
    found: Set[str] = set()
    for m in CALL.finditer(text):
        for lit in call_literals(text, m.end()):
            found |= words_of(lit)
    for m in TAGDEF.finditer(text):
        found |= words_of(m.group(1) or m.group(2) or "")
    return found


def source_files(root: str) -> Iterable[str]:
    for rel in SCAN:
        for dirpath, dirs, files in os.walk(os.path.join(root, rel)):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("build"))
            for name in sorted(files):
                if name.endswith(EXTS):
                    yield os.path.join(dirpath, name)


def extract(root: str = ROOT) -> List[str]:
    found: Set[str] = set()
    for path in source_files(root):
        with open(path, encoding="utf-8", errors="replace") as f:
            found |= file_words(f.read())
    return sorted(found)


def load(path: str = VOCAB_FILE) -> frozenset:
    try:
        with open(path, encoding="utf-8") as f:
            return frozenset(w.strip() for w in f if w.strip())
    except OSError:
        return frozenset()      # fail closed: no vocabulary prints nothing


def main(argv: List[str]) -> int:
    words = extract()
    text = "\n".join(words) + "\n"
    if "--write" in argv:
        with open(VOCAB_FILE, "w", encoding="utf-8") as f:
            f.write(text)
        print("%d words" % len(words))
        return 0
    if "--check" in argv:
        try:
            with open(VOCAB_FILE, encoding="utf-8") as f:
                same = f.read() == text
        except OSError:
            same = False
        print("soak_vocab: %s" % ("up to date" if same else "stale: run tools/soak_vocab.py --write"))
        return 0 if same else 1
    print(text, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
