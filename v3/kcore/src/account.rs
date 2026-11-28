//! Per-process CPU accounting and what termination gives back (#618). Pure logic: the kernel feeds it the low word of
//! mcycle at the points where a process starts and stops running, and zeroes the ranges `reclaim_ranges` names.
//!
//! The cycle counter is the low 32 bits of mcycle, so a span is measured with a wrapping subtraction and is only
//! right while it is shorter than 2^32 cycles (107 s at 40 MHz). A process cannot run that long between two traps:
//! the 1 ms tick ends every span.
use crate::image::Slot;

/// CPU time of one process: cycles spent in U mode between the point the kernel resumed it and its next trap entry.
/// The kernel's own work (trap entry to the next resume) is nobody's: it shows up as the gap between the sum of all
/// meters and the wall clock.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Meter {
    cycles: u64,
    resumed: u32,
}
impl Meter {
    pub const fn new() -> Self {
        Self { cycles: 0, resumed: 0 }
    }
    /// The kernel hands the CPU to this process at `now` (mcycle, low word).
    pub fn resume(&mut self, now: u32) {
        self.resumed = now;
    }
    /// The process trapped at `entry` (the stamp trap.S takes at trap entry). Returns the total so far. Charging again
    /// without a `resume` in between adds nothing, so a second look at the same trap cannot double count.
    pub fn charge(&mut self, entry: u32) -> u64 {
        self.cycles += entry.wrapping_sub(self.resumed) as u64;
        self.resumed = entry;
        self.cycles
    }
    pub fn cycles(&self) -> u64 {
        self.cycles
    }
}
impl Default for Meter {
    fn default() -> Self {
        Self::new()
    }
}

/// True once `cycles` is past `budget` (None: no budget). The check runs at every trap, so a process that yields
/// before each tick cannot hide from it the way it can from a tick count.
pub fn over_cycles(cycles: u64, budget: Option<u64>) -> bool {
    budget.is_some_and(|b| cycles > b)
}

/// What a terminated process leaves behind in its slot: the loaded image in the text region and the whole data region
/// (data, bss, heap, stack). Both ranges are `(address, length)`, always inside the slot, so a bad image length can
/// never make the kernel zero a neighbour.
pub fn reclaim_ranges(slot: &Slot, image_len: usize) -> [(usize, usize); 2] {
    [(slot.text, image_len.min(slot.len)), (slot.data, slot.len)]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_span_is_the_distance_from_resume_to_entry() {
        let mut m = Meter::new();
        m.resume(1000);
        assert_eq!(m.charge(41_000), 40_000);
        m.resume(50_000);
        assert_eq!(m.charge(50_100), 40_100);
    }

    #[test]
    fn a_span_across_the_32_bit_wrap_is_still_right() {
        let mut m = Meter::new();
        m.resume(u32::MAX - 9);
        assert_eq!(m.charge(30), 40);
    }

    #[test]
    fn charging_twice_without_a_resume_counts_once() {
        let mut m = Meter::new();
        m.resume(0);
        m.charge(500);
        assert_eq!(m.charge(500), 500);
    }

    #[test]
    fn the_total_outgrows_32_bits() {
        let mut m = Meter::new();
        for _ in 0..3 {
            m.resume(0);
            m.charge(u32::MAX);
        }
        assert_eq!(m.cycles(), 3 * u32::MAX as u64);
    }

    #[test]
    fn budget_is_exceeded_only_past_it() {
        assert!(!over_cycles(400_000, Some(400_000)));
        assert!(over_cycles(400_001, Some(400_000)));
        assert!(!over_cycles(u64::MAX, None));
    }

    #[test]
    fn reclaim_covers_the_whole_data_region_and_the_image() {
        let s = Slot { text: 0x4FF6_0000, data: 0x4FF7_0000, len: 0x8000 };
        assert_eq!(reclaim_ranges(&s, 900), [(0x4FF6_0000, 900), (0x4FF7_0000, 0x8000)]);
    }

    #[test]
    fn reclaim_never_leaves_the_slot_whatever_the_image_length() {
        let s = Slot { text: 0x4FF6_0000, data: 0x4FF7_0000, len: 0x8000 };
        for len in [0, 1, 0x8000, 0x8001, usize::MAX] {
            let [t, d] = reclaim_ranges(&s, len);
            assert_eq!(t.0, s.text);
            assert!(t.1 <= s.len, "text range {len:#x}");
            assert_eq!(d, (s.data, s.len));
        }
    }
}
