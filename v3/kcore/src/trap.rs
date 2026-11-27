//! Trap and timer arithmetic (#612 checkpoint 1), hardware-free so it is host-tested.
//!
//! The P4 runs the CLIC (mtvec mode 3, hardwired). In CLIC mode mcause is not the plain RISC-V layout: besides the
//! interrupt flag (bit 31) it mirrors minhv (30), mpp (29:28), mpie (27) and mpil (23:16), and the exception or
//! interrupt code is only bits 11:0. Masking with 0x7FFF_FFFF (the classic layout) leaks mpp/mpie/mpil into the code.

/// mcause decoded per the CLIC layout.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Cause {
    pub interrupt: bool,
    /// Exception or interrupt code, bits 11:0.
    pub code: u16,
    /// Previous interrupt level, bits 23:16 (restored into mintstatus.mil by mret).
    pub mpil: u8,
    pub mpie: bool,
    /// Previous privilege, bits 29:28 (0 = U, 3 = M).
    pub mpp: u8,
    pub minhv: bool,
}

pub const fn decode(mcause: u32) -> Cause {
    Cause {
        interrupt: mcause & (1 << 31) != 0,
        code: (mcause & 0xFFF) as u16,
        mpil: ((mcause >> 16) & 0xFF) as u8,
        mpie: mcause & (1 << 27) != 0,
        mpp: ((mcause >> 28) & 3) as u8,
        minhv: mcause & (1 << 30) != 0,
    }
}

pub const EXC_ILLEGAL: u16 = 2;
pub const EXC_ECALL_U: u16 = 8;
pub const EXC_ECALL_M: u16 = 11;
/// The CLINT machine timer is CLIC interrupt id 7 on the P4.
pub const IRQ_MTIMER: u16 = 7;

/// What a U ecall asks for (a7). Anything else is an error for the caller, never an exit.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum UCall {
    Echo,
    Exit,
    Unknown,
}
pub const fn classify_ucall(a7: usize) -> UCall {
    match a7 {
        0 => UCall::Echo,
        1 => UCall::Exit,
        _ => UCall::Unknown,
    }
}
/// a0 for an unknown call: -ENOSYS.
pub const ENOSYS_RET: usize = (-38isize) as usize;

/// Short name of a standard exception code, for fault dumps.
pub const fn exception_name(code: u16) -> &'static str {
    match code {
        0 => "insn-misaligned",
        1 => "insn-access-fault",
        2 => "illegal-insn",
        3 => "breakpoint",
        4 => "load-misaligned",
        5 => "load-access-fault",
        6 => "store-misaligned",
        7 => "store-access-fault",
        8 => "ecall-u",
        11 => "ecall-m",
        12 => "insn-page-fault",
        13 => "load-page-fault",
        15 => "store-page-fault",
        _ => "unknown",
    }
}

/// The next compare value of a periodic tick. It advances from the previous compare, not from "now", so handler
/// latency never accumulates into drift. If the handler ran so late that the next deadline has already passed, it
/// skips whole periods to the first deadline after `now` and reports how many it skipped.
/// `period` must be nonzero.
pub const fn next_deadline(prev_cmp: u64, period: u64, now: u64) -> (u64, u64) {
    debug_assert!(period > 0);
    let next = prev_cmp + period;
    if next > now {
        return (next, 0);
    }
    let behind = (now - next) / period + 1;
    (next + behind * period, behind)
}

/// Running statistics of a periodic event: the gap between consecutive events and the latency of each event behind
/// its deadline, both in mtime ticks.
#[derive(Clone, Copy, Debug)]
pub struct PeriodStats {
    pub count: u32,
    pub missed: u64,
    last: Option<u64>,
    pub gap_min: u64,
    pub gap_max: u64,
    gap_sum: u64,
    pub lat_min: u64,
    pub lat_max: u64,
}
impl Default for PeriodStats {
    fn default() -> Self {
        Self::new()
    }
}
impl PeriodStats {
    pub const fn new() -> Self {
        Self {
            count: 0,
            missed: 0,
            last: None,
            gap_min: u64::MAX,
            gap_max: 0,
            gap_sum: 0,
            lat_min: u64::MAX,
            lat_max: 0,
        }
    }
    /// One event seen at `now` for the deadline `deadline` (now >= deadline on real hardware; saturates otherwise).
    pub fn record(&mut self, deadline: u64, now: u64) {
        let lat = now.saturating_sub(deadline);
        self.lat_min = self.lat_min.min(lat);
        self.lat_max = self.lat_max.max(lat);
        if let Some(prev) = self.last {
            let gap = now.saturating_sub(prev);
            self.gap_min = self.gap_min.min(gap);
            self.gap_max = self.gap_max.max(gap);
            self.gap_sum += gap;
        }
        self.last = Some(now);
        self.count += 1;
    }
    pub fn add_missed(&mut self, n: u64) {
        self.missed += n;
    }
    /// Mean gap in ticks, None before two events.
    pub fn gap_mean(&self) -> Option<u64> {
        (self.count >= 2).then(|| self.gap_sum / (self.count as u64 - 1))
    }
    /// Stable: at least two events, none missed, and every gap within `tol` ticks of `period`.
    pub fn stable(&self, period: u64, tol: u64) -> bool {
        self.count >= 2 && self.missed == 0 && self.gap_min + tol >= period && self.gap_max <= period + tol
    }
}

/// Range and alignment test for the boot-info pointer in a1, run before it is ever dereferenced. Under load-ram the ROM
/// leaves junk in a1 (0x14 was seen, #611), so non-null means nothing: the pointer must lie, with the whole struct,
/// inside L2MEM and be 4-byte aligned. The magic is checked only after this passes.
pub const fn boot_info_in_range(ptr: usize, size: usize, l2_base: usize, l2_end: usize) -> bool {
    ptr.is_multiple_of(4) && ptr >= l2_base && size <= l2_end - l2_base && ptr <= l2_end - size
}

#[cfg(test)]
mod tests {
    #[test]
    fn unknown_ecalls_are_not_exits() {
        use super::*;
        assert_eq!(classify_ucall(0), UCall::Echo);
        assert_eq!(classify_ucall(1), UCall::Exit);
        assert_eq!(classify_ucall(2), UCall::Unknown);
        assert_eq!(classify_ucall(usize::MAX), UCall::Unknown);
        assert_eq!(ENOSYS_RET as isize, -38);
    }

    use super::*;
    use proptest::prelude::*;

    #[test]
    fn clic_timer_cause_keeps_its_code() {
        // Timer interrupt taken from U (mpp 0) with mpie set and mpil 0: the classic 0x7FFF_FFFF mask gives 0x0800_0007.
        let c = decode(0x8800_0007);
        assert!(c.interrupt);
        assert_eq!(c.code, IRQ_MTIMER);
        assert!(c.mpie);
        assert_eq!(c.mpp, 0);
        // Taken from M (mpp 3) at mpil 0xff.
        let c = decode(0xB8FF_0007);
        assert_eq!((c.code, c.mpp, c.mpil), (IRQ_MTIMER, 3, 0xFF));
    }

    #[test]
    fn clic_exception_cause() {
        let c = decode(0x3000_0002);
        assert!(!c.interrupt);
        assert_eq!(c.code, EXC_ILLEGAL);
        assert_eq!(c.mpp, 3);
        assert_eq!(exception_name(c.code), "illegal-insn");
    }

    #[test]
    fn tick_maths() {
        assert_eq!(next_deadline(1000, 100, 1050), (1100, 0));
        // Ran past two deadlines (1100 and 1200): the next one is 1300, two skipped.
        assert_eq!(next_deadline(1000, 100, 1250), (1300, 2));
        // Exactly on the next deadline counts as passed.
        assert_eq!(next_deadline(1000, 100, 1100), (1200, 1));
    }

    #[test]
    fn stats_judge_a_steady_tick() {
        let mut s = PeriodStats::new();
        for i in 0..10 {
            s.record(i * 100, i * 100 + 3);
        }
        assert_eq!(s.count, 10);
        assert_eq!(s.gap_mean(), Some(100));
        assert!(s.stable(100, 0));
        assert_eq!((s.lat_min, s.lat_max), (3, 3));
        s.add_missed(1);
        assert!(!s.stable(100, 10));
    }

    #[test]
    fn stats_catch_a_late_tick() {
        let mut s = PeriodStats::new();
        s.record(0, 0);
        s.record(100, 100);
        s.record(200, 260);
        assert!(!s.stable(100, 50));
        assert!(s.stable(100, 60));
    }

    #[test]
    fn boot_info_rom_junk_is_rejected() {
        let (b, e) = (0x4FF0_0000, 0x4FFC_0000);
        assert!(!boot_info_in_range(0x14, 16, b, e));
        assert!(!boot_info_in_range(0, 16, b, e));
        assert!(!boot_info_in_range(0x4FF4_0002, 16, b, e));
        assert!(!boot_info_in_range(e - 8, 16, b, e));
        assert!(boot_info_in_range(0x4FF4_0000, 16, b, e));
        assert!(boot_info_in_range(e - 16, 16, b, e));
        // A size larger than the whole range must not underflow into a pass.
        assert!(!boot_info_in_range(b, e - b + 4, b, e));
        assert!(boot_info_in_range(b, e - b, b, e));
    }

    proptest! {
        #[test]
        fn code_is_low_twelve_bits(m in any::<u32>()) {
            let c = decode(m);
            prop_assert_eq!(c.code as u32, m & 0xFFF);
            prop_assert_eq!(c.interrupt, m >> 31 == 1);
        }

        #[test]
        fn deadline_is_after_now_and_on_the_grid(prev in 0u64..1 << 40, period in 1u64..1 << 20, late in 0u64..1 << 24) {
            let now = prev + late;
            let (next, skipped) = next_deadline(prev, period, now);
            prop_assert!(next > now);
            prop_assert_eq!((next - prev) % period, 0);
            prop_assert_eq!((next - prev) / period, skipped + 1);
            prop_assert!(next - period <= now);
        }

        #[test]
        fn boot_info_in_range_never_overruns(ptr in any::<u32>(), size in 0u32..4096) {
            let (b, e) = (0x4FF0_0000usize, 0x4FFC_0000usize);
            if boot_info_in_range(ptr as usize, size as usize, b, e) {
                prop_assert!(ptr as usize >= b && ptr as usize + size as usize <= e && ptr.is_multiple_of(4));
            }
        }
    }
}
