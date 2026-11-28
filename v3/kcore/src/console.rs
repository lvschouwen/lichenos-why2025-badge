//! Diagnostic console core (#660): line assembler, command parser and reply formatter. Pure logic: the kernel feeds
//! UART bytes in, acts on the parsed `Command`, fills a `Snapshot` and renders the reply. The console never reads
//! kernel state itself, so it cannot block the scheduler or race the apps.
//!
//! # Reply format (parsed by the bench tool, #661)
//! A reply is one or more lines, each `tag key=value key=value ...`, fields separated by one space, lines ended by
//! `\n`. The first word is the record tag; values never contain a space or `=` (text values are sanitised: bytes
//! outside printable ASCII, space and `=` become `_`, an empty text becomes `-`). Integers are decimal, except fault
//! `cause`/`pc`/`tval` which are `0x` + 8 lowercase hex digits. Every reply ends with exactly one terminator line:
//! `ok`, or `err reason=<token>` (reasons: see `ParseError::reason` and `LineError::reason`). Keys and their order
//! are stable; new keys are only ever appended to a line.
//!
//! ```text
//! ver kernel=<s> stage=<s> build=<s>          boot slot=<n> reset=<s> count=<n>      up ms=<n>
//! clk cpu_hz=<n> psram_hz=<n>                 ipc sent=<n> recv=<n> dropped=<n> syscalls=<n>
//! wdt enabled=<0|1> timeout_ms=<n> feeds=<n> since_feed_ms=<n>
//! mem pools=<n> procs=<n>   then n x  pool name=<s> total=<n> used=<n> peak=<n>
//!                           then m x  memproc pid=<n> name=<s> used=<n>
//! ps count=<n>              then n x  proc pid=<n> name=<s> state=<ready|run|blocked|zombie> cycles=<n>
//! faults count=<n>          then n x  fault idx=<n> pid=<n> cause=0x.. pc=0x.. tval=0x.. cycle=<n>   (newest first)
//! run app=<s> state=requested         kill pid=<n> state=requested      help cmds=<a,b,...>
//! ```
use core::fmt::{self, Display, Write};

/// Longest accepted line, terminator excluded.
pub const LINE_MAX: usize = 96;
/// Longest app name `run` accepts.
pub const APP_NAME_MAX: usize = 16;
/// Highest pid `kill` accepts; pid 0 is the kernel and never killable.
pub const PID_MAX: u8 = 64;

/// Why a received line was dropped before parsing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LineError {
    Overflow,
    NonPrintable,
}
impl LineError {
    pub const fn reason(self) -> &'static str {
        match self {
            Self::Overflow => "line_too_long",
            Self::NonPrintable => "bad_byte",
        }
    }
}

/// Result of feeding one byte to the assembler.
#[derive(Debug, PartialEq, Eq)]
pub enum Feed<'a> {
    /// Nothing to act on yet (or an empty line).
    Pending,
    /// A complete printable-ASCII line, valid until the next `feed`.
    Line(&'a str),
    /// The line just ended was discarded.
    Error(LineError),
}

/// Fixed-buffer line assembler. CR or LF ends a line (an empty line is ignored, so CRLF works), BS and DEL erase the
/// last byte, bytes outside 0x20..=0x7e poison the line. A poisoned or overflowing line is swallowed up to its
/// terminator and reported once there, so the caller sends exactly one error reply.
pub struct LineAssembler<const N: usize = LINE_MAX> {
    buf: [u8; N],
    len: usize,
    bad: Option<LineError>,
    ready: bool,
}
impl<const N: usize> LineAssembler<N> {
    pub const fn new() -> Self {
        Self { buf: [0; N], len: 0, bad: None, ready: false }
    }
    pub fn feed(&mut self, b: u8) -> Feed<'_> {
        if self.ready {
            self.ready = false;
            self.len = 0;
        }
        match b {
            b'\r' | b'\n' => return self.finish(),
            8 | 0x7f => self.len = self.len.saturating_sub(1),
            0x20..=0x7e => self.push(b),
            _ => self.bad = self.bad.or(Some(LineError::NonPrintable)),
        }
        Feed::Pending
    }
    fn push(&mut self, b: u8) {
        if self.bad.is_some() {
            return;
        }
        if self.len < N {
            self.buf[self.len] = b;
            self.len += 1;
        } else {
            self.bad = Some(LineError::Overflow);
        }
    }
    fn finish(&mut self) -> Feed<'_> {
        if let Some(e) = self.bad.take() {
            self.len = 0;
            return Feed::Error(e);
        }
        if self.len == 0 {
            return Feed::Pending;
        }
        self.ready = true;
        // Printable ASCII only, so this cannot fail; the fallback keeps the path panic-free.
        Feed::Line(core::str::from_utf8(&self.buf[..self.len]).unwrap_or(""))
    }
}
impl<const N: usize> Default for LineAssembler<N> {
    fn default() -> Self {
        Self::new()
    }
}

/// A validated app name: 1..=16 bytes of `a-z 0-9 _ -`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AppName {
    buf: [u8; APP_NAME_MAX],
    len: u8,
}
impl AppName {
    pub fn new(s: &str) -> Option<Self> {
        let b = s.as_bytes();
        let ok = !b.is_empty()
            && b.len() <= APP_NAME_MAX
            && b.iter().all(|c| matches!(c, b'a'..=b'z' | b'0'..=b'9' | b'_' | b'-'));
        if !ok {
            return None;
        }
        let mut buf = [0; APP_NAME_MAX];
        buf[..b.len()].copy_from_slice(b);
        Some(Self { buf, len: b.len() as u8 })
    }
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.len as usize]).unwrap_or("")
    }
}

/// A parsed command. `Run` and `Kill` are requests: the kernel acts, the console does not.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Command {
    Ver,
    Boot,
    Up,
    Clk,
    Mem,
    Ps,
    Ipc,
    Faults,
    Wdt,
    Help,
    Run(AppName),
    Kill(u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParseError {
    Empty,
    UnknownCommand,
    ExtraArgs,
    MissingArg,
    BadAppName,
    BadPid,
    PidRange,
}
impl ParseError {
    pub const fn reason(self) -> &'static str {
        match self {
            Self::Empty => "empty",
            Self::UnknownCommand => "unknown_command",
            Self::ExtraArgs => "extra_args",
            Self::MissingArg => "missing_arg",
            Self::BadAppName => "bad_app_name",
            Self::BadPid => "bad_pid",
            Self::PidRange => "pid_range",
        }
    }
}

const SIMPLE: [(&str, Command); 10] = [
    ("ver", Command::Ver),
    ("boot", Command::Boot),
    ("up", Command::Up),
    ("clk", Command::Clk),
    ("mem", Command::Mem),
    ("ps", Command::Ps),
    ("ipc", Command::Ipc),
    ("faults", Command::Faults),
    ("wdt", Command::Wdt),
    ("help", Command::Help),
];
const HELP: &str = "ver,boot,up,clk,mem,ps,ipc,faults,wdt,run,kill,help";

fn one_arg(arg: Option<&str>, extra: bool) -> Result<&str, ParseError> {
    match (arg, extra) {
        (None, _) => Err(ParseError::MissingArg),
        (Some(_), true) => Err(ParseError::ExtraArgs),
        (Some(a), false) => Ok(a),
    }
}

/// Parse one line (words split on spaces, commands are case-sensitive).
pub fn parse(line: &str) -> Result<Command, ParseError> {
    let mut words = line.split(' ').filter(|w| !w.is_empty());
    let name = words.next().ok_or(ParseError::Empty)?;
    let arg = words.next();
    let extra = words.next().is_some();
    match name {
        "run" => AppName::new(one_arg(arg, extra)?).map(Command::Run).ok_or(ParseError::BadAppName),
        "kill" => {
            let a = one_arg(arg, extra)?;
            if a.is_empty() || a.len() > 3 || !a.bytes().all(|c| c.is_ascii_digit()) {
                return Err(ParseError::BadPid);
            }
            match a.parse::<u8>() {
                Ok(p) if (1..=PID_MAX).contains(&p) => Ok(Command::Kill(p)),
                _ => Err(ParseError::PidRange),
            }
        }
        _ => {
            let (_, cmd) = SIMPLE.iter().find(|(n, _)| *n == name).ok_or(ParseError::UnknownCommand)?;
            if arg.is_some() {
                return Err(ParseError::ExtraArgs);
            }
            Ok(*cmd)
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProcState {
    Ready,
    Running,
    Blocked,
    Zombie,
}
impl ProcState {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Ready => "ready",
            Self::Running => "run",
            Self::Blocked => "blocked",
            Self::Zombie => "zombie",
        }
    }
}

#[derive(Clone, Copy)]
pub struct VersionInfo<'a> {
    pub kernel: &'a str,
    pub stage: &'a str,
    pub build: &'a str,
}
#[derive(Clone, Copy)]
pub struct BootInfo<'a> {
    pub slot: u8,
    pub reset: &'a str,
    pub count: u32,
}
#[derive(Clone, Copy)]
pub struct ClockInfo {
    pub cpu_hz: u32,
    pub psram_hz: u32,
}
#[derive(Clone, Copy)]
pub struct PoolInfo<'a> {
    pub name: &'a str,
    pub total: u32,
    pub used: u32,
    pub peak: u32,
}
#[derive(Clone, Copy)]
pub struct ProcInfo<'a> {
    pub pid: u8,
    pub name: &'a str,
    pub state: ProcState,
    pub cycles: u64,
    pub mem_used: u32,
}
#[derive(Clone, Copy, Default)]
pub struct IpcInfo {
    pub sent: u32,
    pub recv: u32,
    pub dropped: u32,
    pub syscalls: u32,
}
#[derive(Clone, Copy)]
pub struct FaultRecord {
    pub pid: u8,
    pub cause: u32,
    pub pc: u32,
    pub tval: u32,
    pub cycle: u64,
}
#[derive(Clone, Copy)]
pub struct WdtInfo {
    pub enabled: bool,
    pub timeout_ms: u32,
    pub feeds: u32,
    pub since_feed_ms: u32,
}

/// Everything a reply can show, copied by the kernel at one instant. Slices hold the newest-first fault list and the
/// live pools and processes.
pub struct Snapshot<'a> {
    pub version: VersionInfo<'a>,
    pub boot: BootInfo<'a>,
    pub uptime_ms: u64,
    pub clock: ClockInfo,
    pub pools: &'a [PoolInfo<'a>],
    pub procs: &'a [ProcInfo<'a>],
    pub ipc: IpcInfo,
    pub faults: &'a [FaultRecord],
    pub wdt: WdtInfo,
}

/// A text value made safe for the `key=value` grammar.
struct Tok<'a>(&'a str);
impl Display for Tok<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.0.is_empty() {
            return f.write_char('-');
        }
        self.0.bytes().try_for_each(|b| f.write_char(if b.is_ascii_graphic() && b != b'=' { b as char } else { '_' }))
    }
}

pub fn render_ok<W: Write>(w: &mut W) -> fmt::Result {
    w.write_str("ok\n")
}
pub fn render_error<W: Write>(w: &mut W, reason: &str) -> fmt::Result {
    writeln!(w, "err reason={}", Tok(reason))
}

/// Render the reply to `cmd` from `snap`, ending with `ok`. `Run` and `Kill` only acknowledge the request; the kernel
/// acts and can follow with its own `render_error` instead if it refuses.
pub fn render<W: Write>(cmd: &Command, snap: &Snapshot, w: &mut W) -> fmt::Result {
    match cmd {
        Command::Ver => {
            let v = &snap.version;
            writeln!(w, "ver kernel={} stage={} build={}", Tok(v.kernel), Tok(v.stage), Tok(v.build))?
        }
        Command::Boot => {
            writeln!(w, "boot slot={} reset={} count={}", snap.boot.slot, Tok(snap.boot.reset), snap.boot.count)?
        }
        Command::Up => writeln!(w, "up ms={}", snap.uptime_ms)?,
        Command::Clk => writeln!(w, "clk cpu_hz={} psram_hz={}", snap.clock.cpu_hz, snap.clock.psram_hz)?,
        Command::Mem => render_mem(snap, w)?,
        Command::Ps => render_ps(snap, w)?,
        Command::Ipc => {
            let i = &snap.ipc;
            writeln!(w, "ipc sent={} recv={} dropped={} syscalls={}", i.sent, i.recv, i.dropped, i.syscalls)?
        }
        Command::Faults => render_faults(snap, w)?,
        Command::Wdt => {
            let d = &snap.wdt;
            writeln!(
                w,
                "wdt enabled={} timeout_ms={} feeds={} since_feed_ms={}",
                d.enabled as u8, d.timeout_ms, d.feeds, d.since_feed_ms
            )?
        }
        Command::Help => writeln!(w, "help cmds={HELP}")?,
        Command::Run(a) => writeln!(w, "run app={} state=requested", Tok(a.as_str()))?,
        Command::Kill(p) => writeln!(w, "kill pid={p} state=requested")?,
    }
    render_ok(w)
}

fn render_mem<W: Write>(snap: &Snapshot, w: &mut W) -> fmt::Result {
    writeln!(w, "mem pools={} procs={}", snap.pools.len(), snap.procs.len())?;
    for p in snap.pools {
        writeln!(w, "pool name={} total={} used={} peak={}", Tok(p.name), p.total, p.used, p.peak)?;
    }
    for p in snap.procs {
        writeln!(w, "memproc pid={} name={} used={}", p.pid, Tok(p.name), p.mem_used)?;
    }
    Ok(())
}
fn render_ps<W: Write>(snap: &Snapshot, w: &mut W) -> fmt::Result {
    writeln!(w, "ps count={}", snap.procs.len())?;
    for p in snap.procs {
        writeln!(w, "proc pid={} name={} state={} cycles={}", p.pid, Tok(p.name), p.state.name(), p.cycles)?;
    }
    Ok(())
}
fn render_faults<W: Write>(snap: &Snapshot, w: &mut W) -> fmt::Result {
    writeln!(w, "faults count={}", snap.faults.len())?;
    for (i, f) in snap.faults.iter().enumerate() {
        writeln!(
            w,
            "fault idx={i} pid={} cause={:#010x} pc={:#010x} tval={:#010x} cycle={}",
            f.pid, f.cause, f.pc, f.tval, f.cycle
        )?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use proptest::prelude::*;
    use std::string::String;

    fn lines(bytes: &[u8]) -> std::vec::Vec<std::result::Result<String, LineError>> {
        let mut a = LineAssembler::<8>::new();
        let mut out = std::vec::Vec::new();
        for &b in bytes {
            match a.feed(b) {
                Feed::Pending => {}
                Feed::Line(l) => out.push(Ok(l.into())),
                Feed::Error(e) => out.push(Err(e)),
            }
        }
        out
    }

    #[test]
    fn assembler_basics() {
        assert_eq!(lines(b"ps\n"), [Ok("ps".into())]);
        assert_eq!(lines(b"ps\r\nup\r\n"), [Ok("ps".into()), Ok("up".into())]);
        assert_eq!(lines(b"\n\r\n"), std::vec![]);
        assert_eq!(lines(b"psx\x08\x7f\x08\x08\x08up\n"), [Ok("up".into())]);
        assert_eq!(lines(b"\x08\x08ps\n"), [Ok("ps".into())]);
    }

    #[test]
    fn assembler_overflow_and_recovery() {
        assert_eq!(lines(b"12345678\n"), [Ok("12345678".into())]);
        assert_eq!(lines(b"123456789\nps\n"), [Err(LineError::Overflow), Ok("ps".into())]);
        // Backspace after overflow cannot rescue the line.
        assert_eq!(lines(b"123456789\x08\x08\nps\n"), [Err(LineError::Overflow), Ok("ps".into())]);
    }

    #[test]
    fn assembler_refuses_non_printable() {
        assert_eq!(lines(b"p\x01s\nps\n"), [Err(LineError::NonPrintable), Ok("ps".into())]);
        assert_eq!(lines(b"\tps\n"), [Err(LineError::NonPrintable)]);
        assert_eq!(lines(b"\xffps\n"), [Err(LineError::NonPrintable)]);
        // Non-printable first, then overflow: still one error.
        assert_eq!(lines(b"\x01123456789\n"), [Err(LineError::NonPrintable)]);
    }

    proptest! {
        #[test]
        fn assembler_never_panics_or_overflows(bytes in proptest::collection::vec(any::<u8>(), 0..400)) {
            let mut a = LineAssembler::<8>::new();
            for b in bytes {
                if let Feed::Line(l) = a.feed(b) {
                    prop_assert!(!l.is_empty() && l.len() <= 8);
                    prop_assert!(l.bytes().all(|c| (0x20..=0x7e).contains(&c)));
                }
            }
        }
        #[test]
        fn parse_never_panics(s in "\\PC{0,120}") {
            let _ = parse(&s);
        }
    }

    #[test]
    fn parses_simple_commands() {
        for (n, c) in SIMPLE {
            assert_eq!(parse(n), Ok(c));
            assert_eq!(parse(&std::format!("  {n}  ")), Ok(c));
            assert_eq!(parse(&std::format!("{n} x")), Err(ParseError::ExtraArgs));
        }
        assert_eq!(parse(""), Err(ParseError::Empty));
        assert_eq!(parse("   "), Err(ParseError::Empty));
        assert_eq!(parse("PS"), Err(ParseError::UnknownCommand));
        assert_eq!(parse("reboot"), Err(ParseError::UnknownCommand));
    }

    #[test]
    fn parses_run_and_kill() {
        assert_eq!(parse("run hello-1").map(|c| matches!(c, Command::Run(a) if a.as_str() == "hello-1")), Ok(true));
        assert_eq!(parse("run"), Err(ParseError::MissingArg));
        assert_eq!(parse("run a b"), Err(ParseError::ExtraArgs));
        assert_eq!(parse("run Hello"), Err(ParseError::BadAppName));
        assert_eq!(parse("run ../x"), Err(ParseError::BadAppName));
        assert_eq!(parse("run 12345678901234567"), Err(ParseError::BadAppName));
        assert_eq!(parse("kill 7"), Ok(Command::Kill(7)));
        assert_eq!(parse("kill 64"), Ok(Command::Kill(64)));
        assert_eq!(parse("kill 65"), Err(ParseError::PidRange));
        assert_eq!(parse("kill 0"), Err(ParseError::PidRange));
        assert_eq!(parse("kill 256"), Err(ParseError::PidRange));
        assert_eq!(parse("kill 99999999"), Err(ParseError::BadPid));
        assert_eq!(parse("kill -1"), Err(ParseError::BadPid));
        assert_eq!(parse("kill x"), Err(ParseError::BadPid));
        assert_eq!(parse("kill"), Err(ParseError::MissingArg));
        assert_eq!(parse("kill 1 2"), Err(ParseError::ExtraArgs));
    }

    fn render_to(cmd: &Command, snap: &Snapshot) -> String {
        let mut s = String::new();
        render(cmd, snap, &mut s).unwrap();
        s
    }

    #[test]
    fn renders_every_command() {
        let pools = [
            PoolInfo { name: "kernel", total: 100, used: 40, peak: 50 },
            PoolInfo { name: "psram pool", total: 0, used: 0, peak: 0 },
        ];
        let procs =
            [ProcInfo { pid: 1, name: "a=b", state: ProcState::Running, cycles: 9_000_000_000, mem_used: 4096 }];
        let faults = [FaultRecord { pid: 1, cause: 5, pc: 0x4000_0010, tval: 0, cycle: 77 }];
        let snap = Snapshot {
            version: VersionInfo { kernel: "0.1.0", stage: "", build: "fw-abc" },
            boot: BootInfo { slot: 1, reset: "poweron", count: 3 },
            uptime_ms: 1234,
            clock: ClockInfo { cpu_hz: 400_000_000, psram_hz: 120_000_000 },
            pools: &pools,
            procs: &procs,
            ipc: IpcInfo { sent: 1, recv: 2, dropped: 3, syscalls: 4 },
            faults: &faults,
            wdt: WdtInfo { enabled: true, timeout_ms: 5000, feeds: 8, since_feed_ms: 12 },
        };
        let t = |c| render_to(&c, &snap);
        assert_eq!(t(Command::Ver), "ver kernel=0.1.0 stage=- build=fw-abc\nok\n");
        assert_eq!(t(Command::Boot), "boot slot=1 reset=poweron count=3\nok\n");
        assert_eq!(t(Command::Up), "up ms=1234\nok\n");
        assert_eq!(t(Command::Clk), "clk cpu_hz=400000000 psram_hz=120000000\nok\n");
        assert_eq!(
            t(Command::Mem),
            "mem pools=2 procs=1\npool name=kernel total=100 used=40 peak=50\npool name=psram_pool total=0 used=0 peak=0\nmemproc pid=1 name=a_b used=4096\nok\n"
        );
        assert_eq!(t(Command::Ps), "ps count=1\nproc pid=1 name=a_b state=run cycles=9000000000\nok\n");
        assert_eq!(t(Command::Ipc), "ipc sent=1 recv=2 dropped=3 syscalls=4\nok\n");
        assert_eq!(
            t(Command::Faults),
            "faults count=1\nfault idx=0 pid=1 cause=0x00000005 pc=0x40000010 tval=0x00000000 cycle=77\nok\n"
        );
        assert_eq!(t(Command::Wdt), "wdt enabled=1 timeout_ms=5000 feeds=8 since_feed_ms=12\nok\n");
        assert_eq!(t(Command::Help), "help cmds=ver,boot,up,clk,mem,ps,ipc,faults,wdt,run,kill,help\nok\n");
        assert_eq!(t(parse("run demo").unwrap()), "run app=demo state=requested\nok\n");
        assert_eq!(t(Command::Kill(3)), "kill pid=3 state=requested\nok\n");
    }

    #[test]
    fn renders_errors_and_empty_lists() {
        let mut s = String::new();
        render_error(&mut s, ParseError::PidRange.reason()).unwrap();
        render_error(&mut s, "a b").unwrap();
        assert_eq!(s, "err reason=pid_range\nerr reason=a_b\n");
        assert_eq!(LineError::Overflow.reason(), "line_too_long");
        assert_eq!(LineError::NonPrintable.reason(), "bad_byte");
    }
}
