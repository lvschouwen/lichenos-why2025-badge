//! The console: UART0 as the ROM left it, the bench-log protocol (CHECK, PERF, END) and the run's verdict counters.
use core::fmt::{self, Write};
use core::sync::atomic::{AtomicU32, Ordering::Relaxed};
use lichen_kcore::check::{write_check, Verdict};
use lichen_perflab::line::{write_perf, Val};
use lichen_platform_esp32p4::uart;

pub struct Con;
impl Write for Con {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                put(b'\r');
            }
            put(b);
        }
        Ok(())
    }
}
fn put(b: u8) {
    while !uart::tx_ready() {}
    uart::put_byte(b);
}

/// A free-form line (anything that is not CHECK, PERF, BEGIN or END is noise to the reports).
macro_rules! pl {
    ($($a:tt)*) => {{ let _ = { use core::fmt::Write as _; writeln!($crate::con::Con, $($a)*) }; }};
}

static FAILS: AtomicU32 = AtomicU32::new(0);

pub fn check(id: &str, v: Verdict, detail: fmt::Arguments) {
    if v == Verdict::Fail {
        FAILS.fetch_add(1, Relaxed);
    }
    if write_check(&mut Con, id, v, detail).is_err() {
        pl!("probe-perf: bad check id {id}");
    }
}
pub fn pass_if(ok: bool, id: &str, detail: fmt::Arguments) {
    check(id, if ok { Verdict::Pass } else { Verdict::Fail }, detail);
}
pub fn perf(metric: &str, kv: &[(&str, Val)]) {
    if write_perf(&mut Con, metric, kv).is_err() {
        pl!("probe-perf: bad metric {metric}");
    }
}
pub fn begin(stage: &str) {
    pl!("BEGIN {stage}");
}
pub fn failed() -> bool {
    FAILS.load(Relaxed) != 0
}
/// The reason on the END line (`ok` only when nothing failed).
pub fn end_reason() -> &'static str {
    if failed() {
        "fail"
    } else {
        "ok"
    }
}
