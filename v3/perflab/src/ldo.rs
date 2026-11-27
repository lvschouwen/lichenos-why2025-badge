//! The one voltage step of #648: the PSRAM rail (LDO channel 2, `PMU.ext_ldo[3]`) from the calibrated pair 12/2 to 7/5 (nominal
//! 1912.5 mV against 1950 mV), and the way back. Owner bounds (2026-11-27): at most the BadgeVMS settings, nothing above the
//! calibrated pair, no eFuse writes. Both fields change in ONE masked 32-bit write to `PMU_EXT_LDO_P1_0P1A_ANA_REG`
//! (0x5011_51D4): two separate field writes would pass 7/2 (a PSRAM brownout) or 12/5 (2925 mV, above the absolute maximum).

/// `PMU_EXT_LDO_P1_0P1A_ANA_REG`: DREF [31:28], MUL [25:23]; bits 26 (EN_VDET) and 27 (EN_CUR_LIM) are not ours.
pub const ANA_REG: usize = 0x5011_51D4;
/// `PMU_EXT_LDO_P1_0P1A_REG`: XPD bit 8 must stay 1, TIEH bit 14 must stay 0 (1 would put the 3.3 V rail on the pin).
pub const LDO_REG: usize = 0x5011_51D0;
pub const MASK: u32 = 0xF380_0000;
/// Settle time before the read-back (`ESP_LDO_VOLTAGE_STABLE_DELAY_US`).
pub const SETTLE_US: u32 = 200;
/// No pair may exceed the calibrated 12/2 (nominal 1950 mV), in tenths of a millivolt.
pub const CEILING_MV_X10: u32 = 19_500;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// dref 12 / mul 2: what the eFuse calibration gives this badge, and what every boot programs.
    Calibrated,
    /// dref 7 / mul 5: the formula's best match for "1900".
    Mv1900,
}
impl Step {
    pub const fn dref(self) -> u32 {
        match self {
            Step::Calibrated => 12,
            Step::Mv1900 => 7,
        }
    }
    pub const fn mul(self) -> u32 {
        match self {
            Step::Calibrated => 2,
            Step::Mv1900 => 5,
        }
    }
    /// The field bits of the ANA word.
    pub const fn bits(self) -> u32 {
        self.dref() << 28 | self.mul() << 23
    }
    pub const fn name(self) -> &'static str {
        match self {
            Step::Calibrated => "cal",
            Step::Mv1900 => "1900",
        }
    }
}

/// Nominal output in tenths of a millivolt: Vref = 0.5 + 0.05 * dref (dref < 9) else 1.0 + 0.1 * (dref - 9), Vout = Vref * (1 + 0.25 * mul).
pub const fn nominal_mv_x10(dref: u32, mul: u32) -> u32 {
    let vref_mv = if dref < 9 { 500 + 50 * dref } else { 1000 + 100 * (dref - 9) };
    vref_mv * (4 + mul) * 10 / 4
}
pub const fn within_ceiling(dref: u32, mul: u32) -> bool {
    nominal_mv_x10(dref, mul) <= CEILING_MV_X10
}

const _: () = {
    assert!(Step::Calibrated.bits() == 0xC100_0000);
    assert!(Step::Mv1900.bits() == 0x7280_0000);
    assert!(Step::Calibrated.bits() & !MASK == 0 && Step::Mv1900.bits() & !MASK == 0);
    assert!(nominal_mv_x10(12, 2) == CEILING_MV_X10);
    assert!(within_ceiling(Step::Mv1900.dref(), Step::Mv1900.mul()));
};

/// The ANA word after a step: both fields replaced, every other bit as it was.
pub const fn ana_after(old: u32, to: Step) -> u32 {
    (old & !MASK) | to.bits()
}

/// The field bits of an ANA word as the rail had them before a step: what the restore puts back (not a hardcoded pair).
pub const fn baseline_bits(ana: u32) -> u32 {
    ana & MASK
}

/// The fields a restore writes: the baseline read before the step, unless it is zero or above the ceiling (a restore that ran before
/// the baseline was stored would otherwise write dref 0 / mul 0): then the calibrated pair.
pub const fn restore_bits(baseline: u32) -> u32 {
    let (dref, mul) = (baseline >> 28, baseline >> 23 & 7);
    if baseline == 0 || baseline & !MASK != 0 || !within_ceiling(dref, mul) {
        Step::Calibrated.bits()
    } else {
        baseline
    }
}

/// What the two words say.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct State {
    pub xpd: u32,
    pub tieh: u32,
    pub dref: u32,
    pub mul: u32,
}
pub const fn decode(ldo: u32, ana: u32) -> State {
    State { xpd: ldo >> 8 & 1, tieh: ldo >> 14 & 1, dref: ana >> 28 & 0xF, mul: ana >> 23 & 7 }
}
impl State {
    /// Rail on, 3.3 V tie off, the pair is `s`.
    pub const fn is(&self, s: Step) -> bool {
        self.xpd == 1 && self.tieh == 0 && self.dref == s.dref() && self.mul == s.mul()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refuse {
    /// The rail is not at the calibrated pair (a first step starts from 12/2 only).
    NotAtCalibrated,
    /// XPD is 0 or TIEH is 1.
    RailState,
    /// A pair above the ceiling (cannot happen with `Step`, kept as the last line of defence).
    AboveCeiling,
}

/// May the step be written now? Only from the calibrated pair with the rail on and TIEH off.
pub const fn plan(now: &State, to: Step) -> Result<(), Refuse> {
    if !within_ceiling(to.dref(), to.mul()) {
        return Err(Refuse::AboveCeiling);
    }
    if now.xpd != 1 || now.tieh != 0 {
        return Err(Refuse::RailState);
    }
    if !(now.dref == Step::Calibrated.dref() && now.mul == Step::Calibrated.mul()) {
        return Err(Refuse::NotAtCalibrated);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn words_of_the_two_steps() {
        assert_eq!(Step::Calibrated.bits(), 0xC100_0000);
        assert_eq!(Step::Mv1900.bits(), 0x7280_0000);
        assert_eq!(MASK, 0xF380_0000);
        assert_eq!(MASK, 0xF000_0000 | 0x0380_0000); // dref [31:28] | mul [25:23], bits 26 and 27 outside
    }

    #[test]
    fn other_bits_survive_the_write() {
        // EN_CUR_LIM (27) and EN_VDET (26) set, the calibrated pair: the step changes only the two fields
        let old = 0xC100_0000 | 1 << 27 | 1 << 26 | 0x0000_1234;
        let new = ana_after(old, Step::Mv1900);
        assert_eq!(new, 0x7280_0000 | 1 << 27 | 1 << 26 | 0x0000_1234);
        assert_eq!(new & !MASK, old & !MASK);
        // and back
        assert_eq!(ana_after(new, Step::Calibrated), old);
        // idempotent
        assert_eq!(ana_after(new, Step::Mv1900), new);
    }

    #[test]
    fn nominal_voltages() {
        assert_eq!(nominal_mv_x10(12, 2), 19_500); // 1950 mV
        assert_eq!(nominal_mv_x10(7, 5), 19_125); // 1912.5 mV
        assert_eq!(nominal_mv_x10(6, 5), 18_000); // the formula's "1800"
                                                  // a real raise is refused: 10/4 is 2100 mV, 13/2 is 2100 mV, 12/3 is 2275 mV
        assert!(!within_ceiling(10, 4));
        assert!(!within_ceiling(13, 2));
        assert!(!within_ceiling(12, 3));
        assert!(within_ceiling(12, 2));
        // the intermediate pairs of a two-write change are exactly what the single write avoids
        assert!(within_ceiling(7, 2) && nominal_mv_x10(7, 2) < 14_000); // a brownout
        assert!(!within_ceiling(12, 5)); // 2925 mV
    }

    #[test]
    fn the_baseline_is_exactly_the_two_fields() {
        let ana = 0x7280_0000 | 1 << 27 | 1 << 26 | 0x55;
        assert_eq!(baseline_bits(ana), 0x7280_0000);
        // restoring it into the stepped word gives back the original fields and keeps the other bits
        assert_eq!(ana_after(0xC100_0000 | 1 << 27, Step::Mv1900) & !MASK, 1 << 27);
    }

    #[test]
    fn a_missing_or_bad_baseline_restores_the_calibrated_pair() {
        assert_eq!(restore_bits(Step::Mv1900.bits()), Step::Mv1900.bits());
        assert_eq!(restore_bits(Step::Calibrated.bits()), Step::Calibrated.bits());
        assert_eq!(restore_bits(0), Step::Calibrated.bits()); // dref 0 / mul 0 would be a brownout
        assert_eq!(restore_bits(10 << 28 | 4 << 23), Step::Calibrated.bits()); // 2100 mV: above the ceiling
        assert_eq!(restore_bits(Step::Mv1900.bits() | 1 << 27), Step::Calibrated.bits());
        // bits outside the fields
    }

    #[test]
    fn the_state_decodes_and_the_plan_guards_it() {
        let cal = decode(1 << 8, Step::Calibrated.bits() | 1 << 27);
        assert!(cal.is(Step::Calibrated) && !cal.is(Step::Mv1900));
        assert_eq!(plan(&cal, Step::Mv1900), Ok(()));
        // the rail off, or the 3.3 V tie on: never touch it
        assert_eq!(plan(&decode(0, Step::Calibrated.bits()), Step::Mv1900), Err(Refuse::RailState));
        assert_eq!(plan(&decode(1 << 8 | 1 << 14, Step::Calibrated.bits()), Step::Mv1900), Err(Refuse::RailState));
        // not at 12/2 (already stepped, or an unexpected baseline)
        assert_eq!(plan(&decode(1 << 8, Step::Mv1900.bits()), Step::Mv1900), Err(Refuse::NotAtCalibrated));
        assert_eq!(plan(&decode(1 << 8, 6 << 28 | 5 << 23), Step::Mv1900), Err(Refuse::NotAtCalibrated));
        // reset state of the register: dref 10, mul 0
        assert_eq!(plan(&decode(1 << 8, 10 << 28), Step::Mv1900), Err(Refuse::NotAtCalibrated));
    }
}
