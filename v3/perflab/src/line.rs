//! The PERF line grammar of #643 (`v3/perf/README.md`): `PERF <metric.dotted> key=value ...`, decimal numbers only, the unit in
//! the key name. A later line for the same metric replaces an earlier one in `perf_report.py`, so a variant goes into the metric
//! name and the knobs go into numeric keys.
use crate::fmt::Fx2;
use core::fmt::{self, Write};

#[derive(Clone, Copy, Debug)]
pub enum Val {
    U(u64),
    I(i64),
    /// Hundredths: `F2(1234)` prints `12.34`.
    F2(i64),
}
impl fmt::Display for Val {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Val::U(v) => write!(f, "{v}"),
            Val::I(v) => write!(f, "{v}"),
            Val::F2(v) => write!(f, "{}", Fx2(v)),
        }
    }
}

/// Metric names are `[a-z0-9_.]`, start with a letter, and have no empty part.
pub fn valid_metric(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 64
        && s.as_bytes()[0].is_ascii_lowercase()
        && s.split('.').all(|p| !p.is_empty())
        && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_' || b == b'.')
}
pub fn valid_key(s: &str) -> bool {
    !s.is_empty() && s.len() <= 32 && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
}

/// Write one PERF line. `Err` (and nothing written) for a name that would break the grammar.
pub fn write_perf(w: &mut impl Write, metric: &str, kv: &[(&str, Val)]) -> fmt::Result {
    if !valid_metric(metric) || kv.is_empty() || !kv.iter().all(|(k, _)| valid_key(k)) {
        return Err(fmt::Error);
    }
    write!(w, "PERF {metric}")?;
    for (k, v) in kv {
        write!(w, " {k}={v}")?;
    }
    w.write_char('\n')
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::String;

    #[test]
    fn a_line_in_the_643_grammar() {
        let mut s = String::new();
        write_perf(
            &mut s,
            "lab.mem.psram.seq_read",
            &[("mb_s", Val::F2(9312)), ("cpu_mhz", Val::U(360)), ("d", Val::I(-4))],
        )
        .unwrap();
        assert_eq!(s, "PERF lab.mem.psram.seq_read mb_s=93.12 cpu_mhz=360 d=-4\n");
    }

    #[test]
    fn bad_names_write_nothing() {
        let mut s = String::new();
        for m in ["", "Lab.x", "lab..x", ".lab", "lab.x.", "1lab", "lab x", "lab.x=1"] {
            assert!(write_perf(&mut s, m, &[("a", Val::U(1))]).is_err(), "{m}");
        }
        assert!(write_perf(&mut s, "lab.x", &[("A", Val::U(1))]).is_err());
        assert!(write_perf(&mut s, "lab.x", &[("a b", Val::U(1))]).is_err());
        assert!(write_perf(&mut s, "lab.x", &[]).is_err());
        assert_eq!(s, "");
    }
}
