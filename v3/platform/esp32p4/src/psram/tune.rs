//! PSRAM timing tuning by DQS phase and delay line, ported from ESP-IDF v6.1 `esp_hw_support/mspi/mspi_timing_tuning/`
//! (`mspi_timing_tuning.c`: s_sweep_for_success_sample_points, s_find_max_consecutive_success_points; and
//! `tuning_scheme_impl/mspi_timing_by_dqs.c`). The reference data is written at 20 MHz, then the phase (4 settings,
//! 1 read each) and the delay lines (31 settings, 100 reads each) are swept at the target clock; the best point is the
//! first good phase, then the middle of the longest run of fully good delay lines.

/// MSPI_TIMING_TEST_DATA_LEN
pub const TEST_BYTES: usize = 128;
pub const TEST_WORDS: usize = TEST_BYTES / 4;
/// MSPI_TIMING_PSRAM_TEST_DATA_ADDR
pub const TEST_ADDR: u32 = 0x80;
/// MSPI_TIMING_DELAYLINE_TEST_NUMS
pub const DELAYLINE_TESTS: u32 = 100;
/// `s_test_data` of mspi_timing_by_dqs.c.
pub const REFERENCE: [u32; TEST_WORDS] = [
    0x7f78_6655,
    0xa5ff_005a,
    0x3f3c_33aa,
    0xa5ff_5a00,
    0x1f1e_9955,
    0xa500_5aff,
    0x0f0f_ccaa,
    0xa55a_00ff,
    0x0787_6655,
    0xffa5_5a00,
    0x03c3_33aa,
    0xff00_a55a,
    0x01e1_9955,
    0xff00_5aa5,
    0x00f0_ccaa,
    0xff5a_00a5,
    0x8078_6655,
    0x00a5_ff5a,
    0xc03c_33aa,
    0x00a5_5aff,
    0xe01e_9355,
    0x00ff_5aa5,
    0xf00f_ccaa,
    0x005a_ffa5,
    0xf887_6655,
    0x5aa5_ff00,
    0xfcc3_33aa,
    0x5aff_a500,
    0xfee1_9955,
    0x5a00_a5ff,
    0x11f0_ccaa,
    0x5a00_ffa5,
];

/// The four DQS phases swept, as the register encodes them: 67.5, 78.75, 90 and 101.25 degrees.
pub const PHASES: [u32; 4] = [0, 1, 2, 3];
/// `s_test_delayline_config`: (data delay line, DQS delay line); first the DQS line shrinks from 15 to 1, then the data
/// line grows from 0 to 15.
pub const DELAYLINES: usize = 31;
pub const fn delayline(i: usize) -> (u32, u32) {
    if i < 15 {
        (0, 15 - i as u32)
    } else {
        (i as u32 - 15, 0)
    }
}

/// Longest run of `true` as (length, index of its last element); the first longest run wins; (0, 0) if none.
/// IDF's `s_find_max_consecutive_success_points`.
pub fn longest_run(good: &[bool]) -> (usize, usize) {
    let (mut max, mut end, mut run) = (0, 0, 0);
    for (i, &g) in good.iter().enumerate() {
        if g {
            run += 1;
        } else {
            if run > max {
                max = run;
                end = i - 1;
            }
            run = 0;
        }
    }
    if run > max {
        max = run;
        end = good.len() - 1;
    }
    (max, end)
}

/// `mspi_timing_psram_select_best_tuning_phase`: the first phase of the run; no run falls back to index 0 (a failure).
pub fn best_phase(len: usize, end: usize) -> usize {
    if len == 0 {
        0
    } else {
        end + 1 - len
    }
}

/// `mspi_timing_psram_select_best_tuning_delayline`: the middle of the run; a run of 0 or 1 falls back to index 0.
pub fn best_delayline(len: usize, end: usize) -> usize {
    if len <= 1 {
        0
    } else {
        end - len / 2
    }
}

/// What tuning chose.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Tuned {
    pub phase_id: usize,
    pub phase_ok: bool,
    pub phase_run: usize,
    pub delayline_id: usize,
    pub delayline_ok: bool,
    pub delayline_run: usize,
    pub data_dl: u32,
    pub dqs_dl: u32,
    /// Per delay line, how many of the 100 reads were good (for the bench log).
    pub dl_good: [u8; DELAYLINES],
}

/// Run both sweeps and apply the best point. `read_matches` reads the 128 reference bytes back with the current tuning
/// registers and says whether they equal `REFERENCE`.
///
/// # Safety
/// PSRAM bring-up only: writes the MSPI IOMUX tuning registers; the clock must already be at the target speed and the
/// reference data written at low speed.
pub unsafe fn sweep(read_matches: &mut dyn FnMut() -> bool) -> Tuned {
    use crate::psram::regs::{set_delaylines, set_dqs_phase};
    let mut phase_good = [false; 4];
    for (i, &p) in PHASES.iter().enumerate() {
        set_dqs_phase(p);
        phase_good[i] = read_matches();
    }
    let (plen, pend) = longest_run(&phase_good);
    let phase_id = best_phase(plen, pend);
    let phase = PHASES[phase_id];

    let mut dl_good = [0u8; DELAYLINES];
    let mut dl_all = [false; DELAYLINES];
    for i in 0..DELAYLINES {
        let (data, dqs) = delayline(i);
        set_dqs_phase(phase);
        set_delaylines(data, dqs);
        let mut ok = 0u8;
        for _ in 0..DELAYLINE_TESTS {
            if read_matches() {
                ok += 1;
            }
        }
        dl_good[i] = ok;
        dl_all[i] = ok as u32 == DELAYLINE_TESTS;
    }
    let (dlen, dend) = longest_run(&dl_all);
    let delayline_id = best_delayline(dlen, dend);
    let (data_dl, dqs_dl) = delayline(delayline_id);
    // leave the best point set (mspi_timing_psram_config_set_tuning_regs)
    set_dqs_phase(phase);
    set_delaylines(data_dl, dqs_dl);
    Tuned {
        phase_id,
        phase_ok: plen > 0,
        phase_run: plen,
        delayline_id,
        delayline_ok: dlen > 1,
        delayline_run: dlen,
        data_dl,
        dqs_dl,
        dl_good,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn longest_run_matches_the_idf_comment() {
        // IDF: {1, 1, 0, 0, 1, 1, 1, 0} -> length 3, end index 6
        let a = [true, true, false, false, true, true, true, false];
        assert_eq!(longest_run(&a), (3, 6));
        // trailing run, and ties keep the first
        assert_eq!(longest_run(&[true, false, false, true, true, true, true, true, true]), (6, 8));
        assert_eq!(longest_run(&[true, true, false, true, true]), (2, 1));
        assert_eq!(longest_run(&[false, false]), (0, 0));
        assert_eq!(longest_run(&[]), (0, 0));
    }

    #[test]
    fn best_points() {
        assert_eq!(best_phase(0, 0), 0);
        assert_eq!(best_phase(3, 3), 1); // phases 1..=3 good: the first of the run
        assert_eq!(best_delayline(0, 0), 0);
        assert_eq!(best_delayline(1, 7), 0);
        assert_eq!(best_delayline(11, 20), 15); // run 10..=20: end - len/2
        assert_eq!(best_delayline(31, 30), 15);
    }

    #[test]
    fn delayline_table_matches_the_idf_table() {
        assert_eq!(delayline(0), (0, 15));
        assert_eq!(delayline(14), (0, 1));
        assert_eq!(delayline(15), (0, 0));
        assert_eq!(delayline(16), (1, 0));
        assert_eq!(delayline(30), (15, 0));
        assert_eq!(DELAYLINES, 31);
    }
}
