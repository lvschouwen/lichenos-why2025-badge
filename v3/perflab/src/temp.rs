//! The die temperature sensor (LP_TSENS): raw value to degrees, and the range choice. Constants from IDF
//! `temperature_sensor_ll.h` and `temperature_sensor_periph.c` for the P4. The eFuse calibration delta is NOT applied (the probe reads
//! no more eFuse words than the LDO fields), so the reading is the uncalibrated one: good for a trend and for a thermal stop, not an
//! absolute figure (IDF quotes an error of 1 to 3 degrees per range).

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Range {
    pub offset: i32,
    /// `I2C_SARADC_TSENS_DAC` register value (4 bits).
    pub reg_val: u8,
    pub min_c: i32,
    pub max_c: i32,
}
pub const RANGES: [Range; 5] = [
    Range { offset: -2, reg_val: 5, min_c: 50, max_c: 125 },
    Range { offset: -1, reg_val: 7, min_c: 20, max_c: 100 },
    Range { offset: 0, reg_val: 15, min_c: -10, max_c: 80 },
    Range { offset: 1, reg_val: 11, min_c: -30, max_c: 50 },
    Range { offset: 2, reg_val: 10, min_c: -40, max_c: 20 },
];
/// The range the first reading uses: offset 0, -10 to 80 degrees.
pub const FIRST_RANGE: usize = 2;
/// Stop an experiment above this (degrees C). The owner named "a thermal concern" without a number; this is a conservative
/// choice for a badge with no heat sink and is reported as such.
pub const STOP_C: i32 = 85;

/// Degrees in hundredths: 0.4386 * raw - 27.88 * offset - 20.52.
pub const fn celsius_x100(raw: u32, offset: i32) -> i32 {
    ((raw as i64 * 4386 - offset as i64 * 278_800 - 205_200) / 100) as i32
}

/// The range whose middle is nearest to `c_x100`, among the ranges that contain it with 5 degrees of margin; `cur` if none does.
pub fn best_range(c_x100: i32, cur: usize) -> usize {
    let mut best: Option<(usize, i32)> = None;
    for (i, r) in RANGES.iter().enumerate() {
        let (lo, hi) = ((r.min_c + 5) * 100, (r.max_c - 5) * 100);
        if c_x100 >= lo && c_x100 <= hi {
            let mid = (r.min_c + r.max_c) * 50;
            let d = (c_x100 - mid).abs();
            if best.is_none_or(|(_, bd)| d < bd) {
                best = Some((i, d));
            }
        }
    }
    best.map_or(cur, |(i, _)| i)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn conversion_matches_idf_arithmetic() {
        // raw 100 at offset 0: 43.86 - 20.52 = 23.34 C
        assert_eq!(celsius_x100(100, 0), 2334);
        // the offset shifts by 27.88 C per step
        assert_eq!(celsius_x100(100, -1), 2334 + 2788);
        assert_eq!(celsius_x100(100, 1), 2334 - 2788);
        assert_eq!(celsius_x100(0, 0), -2052);
    }

    #[test]
    fn range_choice() {
        assert_eq!(RANGES[FIRST_RANGE].offset, 0);
        assert_eq!(best_range(4500, 2), 2); // 45 C: offset-0 range (middle 35) beats offset -1 (middle 60)
        assert_eq!(best_range(6000, 2), 1); // 60 C: the middle of the offset -1 range
        assert_eq!(best_range(7800, 2), 0); // 78 C is within 5 C of the offset-0 limit: the hot range fits it best
        assert_eq!(best_range(9000, 2), 0);
        assert_eq!(best_range(-1500, 2), 4); // the cold range is centred nearest
        assert_eq!(best_range(20_000, 2), 2); // nothing contains it: stay
        assert!(STOP_C <= RANGES[0].max_c);
    }
}
