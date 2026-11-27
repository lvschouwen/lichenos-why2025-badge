//! The matrix of #648 as one validated configuration. The probe parses it from build-time options (`LICHEN_PERF_*`) in a const
//! context, so an impossible combination does not build.
use crate::dma::Burst;
use crate::l2::{self, L2Cfg};
use crate::ldo::Step;
use core::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Cfg {
    pub cpu_mhz: u32,
    pub psram_mhz: u32,
    pub l2: L2Cfg,
    pub burst: Burst,
    pub ldo: Step,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refuse {
    Cpu,
    Psram,
    /// 200 MHz needs the #650 speed in the platform's psram code.
    Psram200Pending,
    L2(l2::Refuse),
}

impl Cfg {
    pub const DEFAULT: Cfg =
        Cfg { cpu_mhz: 360, psram_mhz: 120, l2: L2Cfg { kb: 256, line: 64 }, burst: Burst::B32, ldo: Step::Calibrated };
    /// CPU 360 or 400, PSRAM 120 or 160 (200 only after #650), L2 as `L2Cfg::check`.
    pub const fn check(&self) -> Result<(), Refuse> {
        if !(self.cpu_mhz == 360 || self.cpu_mhz == 400) {
            return Err(Refuse::Cpu);
        }
        match self.psram_mhz {
            120 | 160 => {}
            200 => return Err(Refuse::Psram200Pending),
            _ => return Err(Refuse::Psram),
        }
        match self.l2.check() {
            Ok(()) => Ok(()),
            Err(e) => Err(Refuse::L2(e)),
        }
    }
    /// Is this the production default (360 / 120, v3's ROM cache state, calibrated rail, 32 B bursts)?
    pub const fn is_default(&self) -> bool {
        self.cpu_mhz == 360
            && self.psram_mhz == 120
            && self.l2.kb == 256
            && self.l2.line == 64
            && matches!(self.burst, Burst::B32)
            && matches!(self.ldo, Step::Calibrated)
    }
}
/// The tag that goes into image file names and banners, so a stale variant cannot be loaded for another: `cpu360-ps120-l2k256l64-dma32-ldocal`.
impl fmt::Display for Cfg {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "cpu{}-ps{}-l2k{}l{}-dma{}-ldo{}",
            self.cpu_mhz,
            self.psram_mhz,
            self.l2.kb,
            self.l2.line,
            self.burst.bytes(),
            self.ldo.name()
        )
    }
}

/// Decimal digits only; `None` for anything else (const, for the build-time options).
pub const fn parse_u32(s: &str) -> Option<u32> {
    let b = s.as_bytes();
    if b.is_empty() || b.len() > 9 {
        return None;
    }
    let (mut i, mut v) = (0, 0u32);
    while i < b.len() {
        if !b[i].is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b[i] - b'0') as u32;
        i += 1;
    }
    Some(v)
}

/// `cal` or `1900`.
pub const fn parse_ldo(s: &str) -> Option<Step> {
    match s.as_bytes() {
        b"cal" => Some(Step::Calibrated),
        b"1900" => Some(Step::Mv1900),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::ToString;

    #[test]
    fn the_default_is_production_and_valid() {
        assert_eq!(Cfg::DEFAULT.check(), Ok(()));
        assert!(Cfg::DEFAULT.is_default());
        assert_eq!(Cfg::DEFAULT.to_string(), "cpu360-ps120-l2k256l64-dma32-ldocal");
    }

    #[test]
    fn each_knob_is_checked() {
        let d = Cfg::DEFAULT;
        assert_eq!(Cfg { cpu_mhz: 440, ..d }.check(), Err(Refuse::Cpu)); // the owner said no above 400
        assert_eq!(Cfg { cpu_mhz: 400, ..d }.check(), Ok(()));
        assert_eq!(Cfg { psram_mhz: 160, ..d }.check(), Ok(()));
        assert_eq!(Cfg { psram_mhz: 200, ..d }.check(), Err(Refuse::Psram200Pending));
        assert_eq!(Cfg { psram_mhz: 80, ..d }.check(), Err(Refuse::Psram)); // PSRAM stays >= 120
        assert_eq!(Cfg { l2: L2Cfg { kb: 512, line: 64 }, ..d }.check(), Err(Refuse::L2(l2::Refuse::Line512)));
        assert_eq!(Cfg { l2: L2Cfg { kb: 512, line: 128 }, ..d }.check(), Ok(()));
        assert!(!Cfg { burst: Burst::B64, ..d }.is_default());
        assert!(!Cfg { ldo: Step::Mv1900, ..d }.is_default());
    }

    #[test]
    fn option_parsing() {
        assert_eq!(parse_u32("360"), Some(360));
        for bad in ["", "3a", "-1", "1234567890", " 5"] {
            assert_eq!(parse_u32(bad), None, "{bad:?}");
        }
        assert_eq!(parse_ldo("cal"), Some(Step::Calibrated));
        assert_eq!(parse_ldo("1900"), Some(Step::Mv1900));
        assert_eq!(parse_ldo("1950"), None); // 1950 is the calibrated pair on this chip, not a step
        assert_eq!(parse_ldo("2000"), None);
    }
}
