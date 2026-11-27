//! Fixed-point printing: PERF values are plain decimals, and the probe has no floating point.
use core::fmt;

/// A signed value in hundredths, shown as `-12.05`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Fx2(pub i64);
impl fmt::Display for Fx2 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let a = self.0.unsigned_abs();
        write!(f, "{}{}.{:02}", if self.0 < 0 { "-" } else { "" }, a / 100, a % 100)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::format;

    #[test]
    fn hundredths() {
        assert_eq!(format!("{}", Fx2(0)), "0.00");
        assert_eq!(format!("{}", Fx2(5)), "0.05");
        assert_eq!(format!("{}", Fx2(1234)), "12.34");
        assert_eq!(format!("{}", Fx2(-5)), "-0.05");
        assert_eq!(format!("{}", Fx2(-1200)), "-12.00");
        assert_eq!(format!("{}", Fx2(i64::MIN)), "-92233720368547758.08");
    }
}
