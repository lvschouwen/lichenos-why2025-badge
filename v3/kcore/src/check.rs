//! The bench log protocol. One line per result, `CHECK <id> <PASS|FAIL|INFO> <detail>`, and a last line `END <reason>`.
//! `tools/v3_bench_report.py` parses exactly this; a log without END counts as a failed run.
use core::fmt::{self, Write};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Verdict {
    Pass,
    Fail,
    Info,
}
impl Verdict {
    pub const fn as_str(self) -> &'static str {
        match self {
            Verdict::Pass => "PASS",
            Verdict::Fail => "FAIL",
            Verdict::Info => "INFO",
        }
    }
}

/// An id is 1..=48 characters of `[a-z0-9._-]`, so it never needs quoting in a log line.
pub fn valid_id(id: &str) -> bool {
    (1..=48).contains(&id.len())
        && id.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'.' | b'_' | b'-'))
}

/// Forwards to `W`, turning line breaks into spaces so a detail can never fake a second protocol line.
struct OneLine<'a, W: Write>(&'a mut W);
impl<W: Write> Write for OneLine<'_, W> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for (i, part) in s.split(['\n', '\r']).enumerate() {
            if i > 0 {
                self.0.write_char(' ')?;
            }
            self.0.write_str(part)?;
        }
        Ok(())
    }
}

pub fn write_check(w: &mut impl Write, id: &str, v: Verdict, detail: fmt::Arguments) -> fmt::Result {
    if !valid_id(id) {
        return Err(fmt::Error);
    }
    write!(w, "CHECK {id} {} ", v.as_str())?;
    OneLine(w).write_fmt(detail)?;
    w.write_char('\n')
}

pub fn write_end(w: &mut impl Write, reason: &str) -> fmt::Result {
    write!(w, "END ")?;
    OneLine(w).write_str(reason)?;
    w.write_char('\n')
}

/// Counts verdicts so the final END line can say whether the run was clean.
#[derive(Default, Clone, Copy, Debug)]
pub struct Tally {
    pub pass: u32,
    pub fail: u32,
    pub info: u32,
}
impl Tally {
    pub const fn new() -> Self {
        Self { pass: 0, fail: 0, info: 0 }
    }
    pub fn record(&mut self, v: Verdict) {
        let n = match v {
            Verdict::Pass => &mut self.pass,
            Verdict::Fail => &mut self.fail,
            Verdict::Info => &mut self.info,
        };
        *n = n.saturating_add(1);
    }
    pub fn end_reason(&self) -> &'static str {
        if self.fail == 0 {
            "ok"
        } else {
            "fail"
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use proptest::prelude::*;
    use std::string::String;

    #[test]
    fn formats_a_check_line() {
        let mut s = String::new();
        write_check(&mut s, "c6.en", Verdict::Pass, format_args!("level={}", 1)).unwrap();
        assert_eq!(s, "CHECK c6.en PASS level=1\n");
        let mut s = String::new();
        write_end(&mut s, "ok").unwrap();
        assert_eq!(s, "END ok\n");
    }
    #[test]
    fn refuses_a_bad_id() {
        let mut s = String::new();
        for id in ["", "Has Space", "a\nb", "x".repeat(49).as_str()] {
            assert!(write_check(&mut s, id, Verdict::Info, format_args!("")).is_err(), "{id:?}");
        }
        assert!(s.is_empty());
    }
    #[test]
    fn tally_decides_the_end_reason() {
        let mut t = Tally::new();
        t.record(Verdict::Pass);
        t.record(Verdict::Info);
        assert_eq!(t.end_reason(), "ok");
        t.record(Verdict::Fail);
        assert_eq!(t.end_reason(), "fail");
    }
    proptest! {
        /// Whatever the detail holds, the output is exactly one line.
        #[test]
        fn a_detail_cannot_inject_a_line(detail in "(?s).{0,40}") {
            let mut s = String::new();
            write_check(&mut s, "x", Verdict::Info, format_args!("{detail}")).unwrap();
            prop_assert_eq!(s.matches('\n').count(), 1);
            prop_assert!(s.ends_with('\n'));
            prop_assert!(!s.contains('\r'));
        }
    }
}
