//! Syscall forwarding from core 1 to core 0 (#619). Pure logic: the kernel's `forward` (core 1) and `services`
//! (core 0) glue it to the mailbox and the console.
//!
//! The rules it encodes:
//! - A U pointer is checked against the caller's own regions on core 1 (`in_region`, overflow-safe) and its bytes are
//!   copied into a kernel bounce buffer there; core 0 only ever sees the copy and a length, never a U address.
//! - At most one call in flight per process (`Calls`): the caller is blocked until its reply, so a flooding process
//!   can never queue more than one request and never fill the mailbox. Replies are matched by process *and* sequence
//!   number, so a late reply to a call that was cancelled (the process ended, the stage ended) is dropped and counted.
//! - Core 0 prints a forwarded WRITE only when the console ring has room for it on top of a reserve kept for the
//!   kernel's own rows (`fits`); otherwise it holds the reply, which keeps the caller blocked: back-pressure at the
//!   UART's rate instead of dropped log lines. `Lines` puts each process's output on lines of its own.

/// `[ptr, ptr + len)` lies inside `[base, base + size)`, with no overflow anywhere (an empty range must still start
/// inside, or at the end of, the region).
pub fn in_region(ptr: usize, len: usize, base: usize, size: usize) -> bool {
    match (ptr.checked_add(len), base.checked_add(size)) {
        (Some(end), Some(top)) => ptr >= base && end <= top,
        _ => false,
    }
}

/// The calls in flight on one hart, one slot per process.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Calls<const N: usize> {
    seq: u32,
    waiting: [Option<u32>; N],
    /// Replies that matched no call in flight.
    pub stale: u32,
}

impl<const N: usize> Calls<N> {
    pub const fn new() -> Self {
        Self { seq: 0, waiting: [None; N], stale: 0 }
    }

    /// Start a call for `proc`: its fresh sequence number, or None when `proc` already has one in flight (the limit
    /// is one) or is out of range. Sequence numbers are never reused within the table's life (they wrap at 2^32).
    pub fn begin(&mut self, proc: usize) -> Option<u32> {
        let w = self.waiting.get_mut(proc)?;
        if w.is_some() {
            return None;
        }
        self.seq = self.seq.wrapping_add(1);
        *w = Some(self.seq);
        Some(self.seq)
    }

    /// A reply for `proc` with `seq` arrived: true if it answers that process's call in flight (which is then done).
    /// Anything else is stale and counted.
    pub fn complete(&mut self, proc: usize, seq: u32) -> bool {
        match self.waiting.get_mut(proc) {
            Some(w) if *w == Some(seq) => {
                *w = None;
                true
            }
            _ => {
                self.stale = self.stale.saturating_add(1);
                false
            }
        }
    }

    /// `proc` ended or its request never left: forget its call, so a late reply is stale.
    pub fn cancel(&mut self, proc: usize) {
        if let Some(w) = self.waiting.get_mut(proc) {
            *w = None;
        }
    }

    pub fn in_flight(&self, proc: usize) -> bool {
        self.waiting.get(proc).is_some_and(|w| w.is_some())
    }

    pub fn outstanding(&self) -> usize {
        self.waiting.iter().filter(|w| w.is_some()).count()
    }
}

impl<const N: usize> Default for Calls<N> {
    fn default() -> Self {
        Self::new()
    }
}

/// What core 0's console prints of a forwarded WRITE: the printable ASCII bytes and newlines, nothing else.
pub fn printable(b: u8) -> bool {
    b == b'\n' || (0x20..0x7F).contains(&b)
}

/// Per-process line state of the forwarded console: a prefix at the start of each of a process's lines, so its bytes
/// never run into another process's line or into a kernel row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Lines<const N: usize> {
    open: [bool; N],
}

impl<const N: usize> Lines<N> {
    pub const fn new() -> Self {
        Self { open: [false; N] }
    }

    /// Console bytes that `render` would queue for `bytes` from `proc` with a `prefix_len`-byte prefix: a newline
    /// counts two (the console sends CR LF), and printing another process's bytes first closes its open line.
    pub fn cost(&self, proc: usize, bytes: &[u8], prefix_len: usize) -> usize {
        let others = self.open.iter().enumerate().any(|(i, &o)| o && i != proc);
        let mut open = self.open.get(proc).copied().unwrap_or(false);
        let mut n = if others { 2 } else { 0 };
        for &b in bytes.iter().filter(|&&b| printable(b)) {
            if !open {
                n += prefix_len;
                open = true;
            }
            if b == b'\n' {
                n += 2;
                open = false;
            } else {
                n += 1;
            }
        }
        n
    }

    /// Emit `bytes` from `proc`: `prefix()` before each of its lines, `emit(b)` for each printable byte. If another
    /// process has a line open, it is ended first (`emit(b'\n')`). Returns the bytes `emit` saw.
    pub fn render(&mut self, proc: usize, bytes: &[u8], mut prefix: impl FnMut(), mut emit: impl FnMut(u8)) -> usize {
        if proc >= N {
            return 0;
        }
        let mut n = 0;
        if self.open.iter().enumerate().any(|(i, &o)| o && i != proc) {
            emit(b'\n');
            n += 1;
            self.open = [false; N];
        }
        for &b in bytes.iter().filter(|&&b| printable(b)) {
            if !self.open[proc] {
                prefix();
                self.open[proc] = true;
            }
            emit(b);
            n += 1;
            if b == b'\n' {
                self.open[proc] = false;
            }
        }
        n
    }

    /// End every open line (before a kernel row). True if a newline is needed.
    pub fn close(&mut self) -> bool {
        let any = self.open.iter().any(|&o| o);
        self.open = [false; N];
        any
    }
}

impl<const N: usize> Default for Lines<N> {
    fn default() -> Self {
        Self::new()
    }
}

/// A forwarded write of `cost` bytes may go into a console ring with `free` bytes free only if `reserve` bytes stay
/// free after it: the kernel's own rows always find room, whatever an app floods.
pub fn fits(cost: usize, free: usize, reserve: usize) -> bool {
    cost.checked_add(reserve).is_some_and(|need| need <= free)
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use proptest::prelude::*;
    use std::vec::Vec;

    #[test]
    fn a_range_must_lie_inside_the_region() {
        assert!(in_region(0x1000, 0x10, 0x1000, 0x100));
        assert!(in_region(0x10F0, 0x10, 0x1000, 0x100));
        assert!(!in_region(0x10F1, 0x10, 0x1000, 0x100));
        assert!(!in_region(0x0FFF, 0x10, 0x1000, 0x100));
        assert!(in_region(0x1100, 0, 0x1000, 0x100), "an empty range at the end is inside");
        assert!(!in_region(0x1101, 0, 0x1000, 0x100));
    }

    #[test]
    fn a_wrapping_range_or_region_is_refused() {
        assert!(!in_region(usize::MAX - 3, 8, 0, usize::MAX));
        assert!(!in_region(0x1000, 0x10, usize::MAX - 0x10, 0x100));
        assert!(!in_region(4, usize::MAX, 0, 0x100));
    }

    proptest! {
        /// Whatever the numbers, an accepted range is inside the region in exact (u128) arithmetic.
        #[test]
        fn an_accepted_range_is_inside(ptr in any::<u32>(), len in any::<u32>(), base in any::<u32>(), size in any::<u32>()) {
            let (p, l, b, s) = (ptr as usize, len as usize, base as usize, size as usize);
            let exact = p as u128 >= b as u128 && p as u128 + l as u128 <= b as u128 + s as u128;
            if in_region(p, l, b, s) {
                prop_assert!(exact);
            }
        }
    }

    #[test]
    fn one_call_in_flight_per_process() {
        let mut c = Calls::<3>::new();
        let s0 = c.begin(0).unwrap();
        assert_eq!(c.begin(0), None, "a second call while the first is in flight");
        let s1 = c.begin(1).unwrap();
        assert_ne!(s0, s1);
        assert_eq!(c.outstanding(), 2);
        assert!(c.complete(0, s0));
        assert!(!c.in_flight(0));
        assert!(c.begin(0).is_some(), "free again after its reply");
        assert_eq!(c.begin(3), None, "out of range");
    }

    #[test]
    fn a_reply_must_match_process_and_seq() {
        let mut c = Calls::<3>::new();
        let s0 = c.begin(0).unwrap();
        let s1 = c.begin(1).unwrap();
        assert!(!c.complete(1, s0), "another process's seq");
        assert!(!c.complete(0, s0.wrapping_add(7)), "a seq nobody sent");
        assert!(!c.complete(9, s0), "a process out of range");
        assert_eq!(c.stale, 3);
        assert!(c.in_flight(0) && c.in_flight(1), "stale replies leave the calls in flight");
        assert!(c.complete(0, s0) && c.complete(1, s1));
        assert!(!c.complete(0, s0), "a duplicate is stale");
        assert_eq!(c.stale, 4);
    }

    #[test]
    fn a_cancelled_call_turns_its_late_reply_stale() {
        let mut c = Calls::<2>::new();
        let s = c.begin(1).unwrap();
        c.cancel(1);
        let s2 = c.begin(1).unwrap();
        assert!(!c.complete(1, s), "the old call's reply must not answer the new one");
        assert!(c.complete(1, s2));
    }

    #[test]
    fn seqs_stay_unique_across_the_wrap() {
        let mut c = Calls::<1> { seq: u32::MAX - 1, ..Calls::new() };
        let a = c.begin(0).unwrap();
        c.complete(0, a);
        let b = c.begin(0).unwrap();
        assert_eq!((a, b), (u32::MAX, 0));
    }

    fn rendered(l: &mut Lines<3>, proc: usize, s: &[u8]) -> Vec<u8> {
        let out = core::cell::RefCell::new(Vec::new());
        l.render(proc, s, || out.borrow_mut().extend_from_slice(b"[p] "), |b| out.borrow_mut().push(b));
        out.into_inner()
    }

    #[test]
    fn a_prefix_starts_every_line_and_only_lines() {
        let mut l = Lines::<3>::new();
        assert_eq!(rendered(&mut l, 0, b"ab\ncd"), b"[p] ab\n[p] cd");
        assert_eq!(rendered(&mut l, 0, b"e\n"), b"e\n");
        assert!(!l.close());
    }

    #[test]
    fn unprintable_bytes_are_dropped() {
        let mut l = Lines::<3>::new();
        assert_eq!(rendered(&mut l, 1, b"\x1b[2Jx\r\x00\n"), b"[p] [2Jx\n");
        assert_eq!(rendered(&mut l, 1, b"\x07\x7f"), b"", "nothing printable: no prefix either");
    }

    #[test]
    fn another_process_closes_an_open_line() {
        let mut l = Lines::<3>::new();
        assert_eq!(rendered(&mut l, 0, b"..."), b"[p] ...");
        assert_eq!(rendered(&mut l, 2, b"x\n"), b"\n[p] x\n");
        assert_eq!(rendered(&mut l, 0, b"."), b"[p] .");
        assert!(l.close(), "an open line needs its newline before a kernel row");
        assert!(!l.close());
    }

    proptest! {
        /// `cost` is exactly what `render` queues on the console (a newline goes out as CR LF).
        #[test]
        fn cost_is_what_render_emits(steps in proptest::collection::vec((0usize..3, proptest::collection::vec(any::<u8>(), 0..40)), 0..20)) {
            let mut l = Lines::<3>::new();
            for (proc, bytes) in steps {
                let want = l.cost(proc, &bytes, 4);
                let out = rendered(&mut l, proc, &bytes);
                let on_wire = out.len() + out.iter().filter(|&&b| b == b'\n').count();
                prop_assert_eq!(want, on_wire);
            }
        }
    }

    #[test]
    fn a_write_must_leave_the_reserve_free() {
        assert!(fits(100, 1124, 1024));
        assert!(!fits(101, 1124, 1024));
        assert!(!fits(usize::MAX, usize::MAX, 1));
        assert!(fits(0, 1024, 1024));
    }
}
