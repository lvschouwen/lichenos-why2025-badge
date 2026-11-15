"""Split a soak's samples by segment (#476, #407). The use-soak driver writes marker lines into the sampler
transcript: `## segment awake|night <epoch>` at each segment change, `## session <n> <block> start|end <epoch>` around
each session, and the baseline's `## sample <n> <iso> epoch=<epoch>` before each sample.

A sample row (a CSV row of soak_to_csv) belongs to
- `awake` / `night`: the segment in force at its time (from its marker to the next one);
- a block name (chat, contacts, tour, files, web, idle): the span after the start of a session of that block up to and
  including the start of the next session. The driver takes a sample just BEFORE a session at the same second, so that
  sample shows the previous block's effects and belongs to it; the one taken a minute into the gap and the one just
  before the next session belong to this block.
The trend test assumes evenly spaced samples, so on a split (a night, one block) its gap lines are expected.
A row's time is its `epoch` column when it has one, else the transcript's first sample epoch plus its `t` column
(seconds since the first sample, as soak_to_csv writes it). Standard library only."""
import re
from typing import Dict, List, Optional, Tuple

import soak_parse

SEGMENT_RX = re.compile(r"^## segment (awake|night) (\d+)\s*$")
SESSION_RX = re.compile(r"^## session (\d+) (\w+) (start|end) (\d+)\s*$")
SAMPLE_RX = re.compile(r"^## sample \d+ \S+ epoch=(\d+)\s*$")
SEGMENTS = ("awake", "night")


class Markers:
    def __init__(self) -> None:
        self.first_sample: Optional[int] = None
        self.segments: List[Tuple[int, str]] = []            # (epoch, name)
        self.sessions: List[Tuple[int, str, int]] = []       # (n, block, start epoch)

    def blocks(self) -> List[str]:
        return sorted({b for _, b, _ in self.sessions})


def parse_markers(lines) -> Markers:
    m = Markers()
    for line in lines:
        s = SAMPLE_RX.match(line)
        if s and m.first_sample is None:
            m.first_sample = int(s.group(1))
            continue
        g = SEGMENT_RX.match(line)
        if g:
            m.segments.append((int(g.group(2)), g.group(1)))
            continue
        e = SESSION_RX.match(line)
        if e and e.group(3) == "start":
            m.sessions.append((int(e.group(1)), e.group(2), int(e.group(4))))
    m.segments.sort()
    m.sessions.sort(key=lambda x: x[2])
    return m


def sample_epoch(sample: soak_parse.Sample, m: Markers) -> Optional[float]:
    e = soak_parse.value(sample, "epoch")
    if e is not None:
        return e
    t = soak_parse.value(sample, "t")
    if t is None or m.first_sample is None:
        return None
    return m.first_sample + t


def _in_segment(m: Markers, name: str, epoch: float) -> bool:
    for i, (start, seg) in enumerate(m.segments):
        end = m.segments[i + 1][0] if i + 1 < len(m.segments) else float("inf")
        if seg == name and start <= epoch < end:
            return True
    return False


def _in_block(m: Markers, name: str, epoch: float) -> bool:
    for i, (_, block, start) in enumerate(m.sessions):
        end = m.sessions[i + 1][2] if i + 1 < len(m.sessions) else float("inf")
        if block == name and start < epoch <= end:
            return True
    return False


def select(samples: List[soak_parse.Sample], m: Markers, name: str) -> List[soak_parse.Sample]:
    """The samples of segment `name` (awake, night or a block). ValueError when the name is unknown to the markers or a
    row has no time: a silent empty report would read as 'no data'."""
    if name not in SEGMENTS and name not in m.blocks():
        raise ValueError("segment '%s': not awake, night or a block in the transcript (%s)" % (
            name, ", ".join(m.blocks()) or "no sessions"))
    if name in SEGMENTS and not any(s == name for _, s in m.segments):
        raise ValueError("segment '%s': no '## segment %s' marker in the transcript" % (name, name))
    out = []
    for s in samples:
        epoch = sample_epoch(s, m)
        if epoch is None:
            raise ValueError("segment: a sample has no epoch or t column (or the transcript has no sample line)")
        if _in_segment(m, name, epoch) if name in SEGMENTS else _in_block(m, name, epoch):
            out.append(s)
    return out

