//! A single-producer, single-consumer message queue between the two HP cores (#612 checkpoint 3), the base for core 1
//! -> core 0 syscall forwarding (#619): one `Spsc` per direction.
//!
//! Layout: a power-of-two array of `T: Copy` slots and two free-running `u32` indices. `tail` counts messages pushed
//! (written by the producer only), `head` counts messages popped (written by the consumer only). Both wrap at 2^32;
//! `tail - head` (wrapping) is the fill level, always in 0..=N, and slot `i % N` holds message number `i`.
//!
//! Memory ordering (the whole correctness argument; x86 TSO in the host stress test cannot catch a missing
//! Release/Acquire, so it rests on this):
//! - push: own `tail` Relaxed (nobody else writes it); peer `head` **Acquire**, pairing with the consumer's Release
//!   store of `head`: once we see `head = h`, the consumer's reads of every slot below `h` happened before, so we may
//!   overwrite slot `(h - 1) % N` and earlier. Then write the slot and store `tail + 1` **Release**: the slot write is
//!   visible before the new tail.
//! - pop: own `head` Relaxed; peer `tail` **Acquire**, pairing with the producer's Release: the slot below the tail we
//!   saw is fully written. Read the slot, then store `head + 1` **Release**: the read is done before the producer can
//!   see the slot as free.
//!
//! On RV32 (RVWMO) a Release store is `fence rw,w; sw` and an Acquire load is `lw; fence r,rw`; no AMO is needed, so the
//! queue works on any memory the two harts can both load and store. The ESP32-P4 has one L1 data cache shared by
//! both HP cores (the ROM header has a single `CACHE_MAP_L1_DCACHE` id next to `CACHE_MAP_L1_ICACHE_0/1`), and
//! internal L2MEM is reached through it (`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`). Both harts therefore see the same
//! cached line: data needs no write-back or invalidate between harts, only the fences above. Rule: both sides use
//! the cached 0x4FF.. alias only; the uncached 0x8FF.. alias bypasses that shared L1 and can read stale memory.
use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicU32, Ordering};

pub struct Spsc<T: Copy, const N: usize> {
    head: AtomicU32,
    tail: AtomicU32,
    slots: UnsafeCell<[MaybeUninit<T>; N]>,
}

// SAFETY: slots are shared between exactly one producer and one consumer (enforced by `split`); a slot is written only
// while the producer owns it (index not yet published) and read only after the Acquire of its publication.
unsafe impl<T: Copy + Send, const N: usize> Sync for Spsc<T, N> {}

impl<T: Copy, const N: usize> Spsc<T, N> {
    const CAP_OK: () = assert!(N.is_power_of_two() && N <= 1 << 31, "capacity must be a power of two <= 2^31");

    pub const fn new() -> Self {
        Self::starting_at(0)
    }

    /// An empty queue whose indices start at `start` (tests start near `u32::MAX` to cross the wrap).
    pub const fn starting_at(start: u32) -> Self {
        let () = Self::CAP_OK;
        Self {
            head: AtomicU32::new(start),
            tail: AtomicU32::new(start),
            slots: UnsafeCell::new([const { MaybeUninit::uninit() }; N]),
        }
    }

    pub const fn capacity(&self) -> usize {
        N
    }

    /// The producer and the consumer handle.
    ///
    /// # Safety
    /// At most one live `Producer` and one live `Consumer` per queue, over the queue's whole life: call this once (or
    /// again only after both earlier handles are gone). Each handle may move to another hart, but is used by one
    /// context at a time (it is `Send`, not `Sync`).
    pub unsafe fn split(&self) -> (Producer<'_, T, N>, Consumer<'_, T, N>) {
        (Producer { q: self }, Consumer { q: self })
    }

    /// The producer handle alone, for a queue in a static whose two sides start on different harts.
    ///
    /// # Safety
    /// As `split`: at most one live `Producer` for this queue at any time.
    pub unsafe fn producer(&self) -> Producer<'_, T, N> {
        Producer { q: self }
    }

    /// The consumer handle alone.
    ///
    /// # Safety
    /// As `split`: at most one live `Consumer` for this queue at any time.
    pub unsafe fn consumer(&self) -> Consumer<'_, T, N> {
        Consumer { q: self }
    }

    /// Messages waiting, as seen from anywhere (a snapshot; it may be stale by the time it is used).
    pub fn len(&self) -> usize {
        let h = self.head.load(Ordering::Acquire);
        let t = self.tail.load(Ordering::Acquire);
        t.wrapping_sub(h) as usize
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn slot(&self, i: u32) -> *mut MaybeUninit<T> {
        // N is a power of two, so `i % N` stays continuous across the u32 wrap.
        let base = self.slots.get() as *mut MaybeUninit<T>;
        base.wrapping_add(i as usize & (N - 1))
    }
}

impl<T: Copy, const N: usize> Default for Spsc<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

pub struct Producer<'a, T: Copy, const N: usize> {
    q: &'a Spsc<T, N>,
}
pub struct Consumer<'a, T: Copy, const N: usize> {
    q: &'a Spsc<T, N>,
}
// SAFETY: a handle is the sole owner of its side of the queue (see `split`); moving it to another hart is the point.
unsafe impl<T: Copy + Send, const N: usize> Send for Producer<'_, T, N> {}
// SAFETY: as Producer.
unsafe impl<T: Copy + Send, const N: usize> Send for Consumer<'_, T, N> {}

impl<T: Copy, const N: usize> Producer<'_, T, N> {
    /// Queue `m`; gives it back when the queue is full. Never blocks.
    pub fn push(&mut self, m: T) -> Result<(), T> {
        let t = self.q.tail.load(Ordering::Relaxed);
        let h = self.q.head.load(Ordering::Acquire);
        if t.wrapping_sub(h) as usize >= N {
            return Err(m);
        }
        // SAFETY: slot t is outside head..tail, so the consumer does not read it until the Release below publishes it,
        // and the Acquire of head ordered the consumer's last read of it (one lap ago) before this write.
        unsafe { self.q.slot(t).write(MaybeUninit::new(m)) };
        self.q.tail.store(t.wrapping_add(1), Ordering::Release);
        Ok(())
    }

    /// Free slots right now (only grows until the next push).
    pub fn free(&self) -> usize {
        let t = self.q.tail.load(Ordering::Relaxed);
        let h = self.q.head.load(Ordering::Acquire);
        N - t.wrapping_sub(h) as usize
    }
}

impl<T: Copy, const N: usize> Consumer<'_, T, N> {
    /// The oldest message, if any. Never blocks.
    pub fn pop(&mut self) -> Option<T> {
        let h = self.q.head.load(Ordering::Relaxed);
        let t = self.q.tail.load(Ordering::Acquire);
        if h == t {
            return None;
        }
        // SAFETY: h is in head..tail, published by the producer's Release that the Acquire above saw, so the slot is
        // initialised; the producer does not overwrite it until the Release below frees it.
        let m = unsafe { self.q.slot(h).read().assume_init() };
        self.q.head.store(h.wrapping_add(1), Ordering::Release);
        Some(m)
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use proptest::prelude::*;
    use std::collections::VecDeque;
    use std::vec::Vec;

    #[test]
    fn full_and_empty() {
        let q = Spsc::<u32, 4>::new();
        // SAFETY: the only split of this queue.
        let (mut p, mut c) = unsafe { q.split() };
        assert_eq!(c.pop(), None);
        for i in 0..4 {
            assert_eq!(p.push(i), Ok(()));
        }
        assert_eq!((p.push(9), p.free(), q.len()), (Err(9), 0, 4));
        assert_eq!(c.pop(), Some(0));
        assert_eq!((p.free(), p.push(4)), (1, Ok(())));
        let out: Vec<u32> = core::iter::from_fn(|| c.pop()).collect();
        assert_eq!(out, [1, 2, 3, 4]);
        assert!(q.is_empty());
    }

    #[test]
    fn crosses_the_u32_wrap() {
        let q = Spsc::<u64, 8>::starting_at(u32::MAX - 3);
        // SAFETY: the only split of this queue.
        let (mut p, mut c) = unsafe { q.split() };
        for lap in 0..5u64 {
            for i in 0..8 {
                p.push(lap * 100 + i).unwrap();
            }
            assert!(p.push(0).is_err(), "full at lap {lap}");
            for i in 0..8 {
                assert_eq!(c.pop(), Some(lap * 100 + i));
            }
            assert_eq!(c.pop(), None);
        }
    }

    #[test]
    fn capacity_one_works() {
        let q = Spsc::<u8, 1>::new();
        // SAFETY: the only split of this queue.
        let (mut p, mut c) = unsafe { q.split() };
        assert_eq!((p.push(1), p.push(2)), (Ok(()), Err(2)));
        assert_eq!((c.pop(), c.pop()), (Some(1), None));
    }

    /// Two OS threads, one per side, 200k messages through a small queue: none lost, duplicated or reordered. Under
    /// miri (#619, review L-f: `cargo +nightly miri test -p lichen-kcore mailbox`) the same test runs with fewer
    /// messages: miri's data race detector and weak-memory emulation then check the Release/Acquire pairs that x86 TSO
    /// cannot (a Relaxed publication of `tail` or `head` is reported as a data race on the slot).
    #[test]
    fn two_threads_keep_fifo() {
        const MSGS: u32 = if cfg!(miri) { 2_000 } else { 200_000 };
        static Q: Spsc<[u32; 2], 16> = Spsc::starting_at(u32::MAX - 100);
        // SAFETY: the only split of Q.
        let (mut p, mut c) = unsafe { Q.split() };
        let tx = std::thread::spawn(move || {
            for i in 0..MSGS {
                let mut m = [i, !i];
                while let Err(back) = p.push(m) {
                    m = back;
                    std::hint::spin_loop();
                }
            }
        });
        let mut next = 0u32;
        while next < MSGS {
            if let Some([a, b]) = c.pop() {
                assert_eq!((a, b), (next, !next), "torn, lost or reordered");
                next += 1;
            } else {
                std::hint::spin_loop();
            }
        }
        tx.join().unwrap();
        assert_eq!(c.pop(), None);
    }

    /// The queue as the kernel uses it (#619): requests one way, replies the other, a full message struct per slot,
    /// across the u32 wrap. Small enough for miri; the reply must carry what the request carried.
    #[test]
    fn request_reply_round_trips_between_threads() {
        const CALLS: u32 = if cfg!(miri) { 300 } else { 50_000 };
        static REQ: Spsc<[u32; 8], 4> = Spsc::starting_at(u32::MAX - 2);
        static REP: Spsc<[u32; 8], 4> = Spsc::starting_at(u32::MAX - 1);
        // SAFETY: the only split of each queue.
        let ((mut req_tx, mut req_rx), (mut rep_tx, mut rep_rx)) = unsafe { (REQ.split(), REP.split()) };
        let server = std::thread::spawn(move || {
            for _ in 0..CALLS {
                let m = loop {
                    match req_rx.pop() {
                        Some(m) => break m,
                        None => std::hint::spin_loop(),
                    }
                };
                let mut r = m;
                r[1] = m[2..].iter().fold(0u32, |a, &x| a.wrapping_add(x));
                while rep_tx.push(r).is_err() {
                    std::hint::spin_loop();
                }
            }
        });
        for seq in 0..CALLS {
            let m = [seq, 0, seq ^ 1, seq ^ 2, seq ^ 3, seq ^ 4, seq ^ 5, seq ^ 6];
            while req_tx.push(m).is_err() {
                std::hint::spin_loop();
            }
            let r = loop {
                match rep_rx.pop() {
                    Some(r) => break r,
                    None => std::hint::spin_loop(),
                }
            };
            let sum = m[2..].iter().fold(0u32, |a, &x| a.wrapping_add(x));
            assert_eq!((r[0], r[1], &r[2..]), (seq, sum, &m[2..]), "reply {seq}");
        }
        server.join().unwrap();
    }

    proptest! {
        /// Model check against a bounded VecDeque from any start index: same accept/refuse, same FIFO output.
        #[test]
        #[cfg_attr(miri, ignore)] // hundreds of cases: minutes under miri; the threaded tests are miri's target
        fn matches_a_model(start in any::<u32>(), ops in proptest::collection::vec(proptest::option::of(any::<u16>()), 0..300)) {
            let q = Spsc::<u16, 8>::starting_at(start);
            // SAFETY: the only split of this queue.
            let (mut p, mut c) = unsafe { q.split() };
            let mut m: VecDeque<u16> = VecDeque::new();
            for op in ops {
                match op {
                    Some(v) => {
                        let want = if m.len() < 8 { m.push_back(v); Ok(()) } else { Err(v) };
                        prop_assert_eq!(p.push(v), want);
                    }
                    None => prop_assert_eq!(c.pop(), m.pop_front()),
                }
                prop_assert_eq!(q.len(), m.len());
                prop_assert_eq!(p.free(), 8 - m.len());
            }
        }
    }
}
