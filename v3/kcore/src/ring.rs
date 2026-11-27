//! A fixed-size byte ring for the UART log. `push` never blocks and never fails: when the ring is full the oldest
//! byte is dropped and counted, so a stuck UART cannot stall the kernel. Single context only (no atomics): the owner
//! wraps it in a `Global` and keeps the Global invariant.
pub struct Ring<const N: usize> {
    buf: [u8; N],
    head: usize,
    len: usize,
    dropped: u32,
}
impl<const N: usize> Ring<N> {
    pub const fn new() -> Self {
        Self { buf: [0; N], head: 0, len: 0, dropped: 0 }
    }
    pub const fn len(&self) -> usize {
        self.len
    }
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }
    /// Bytes lost to overflow since the last `take_dropped`.
    pub const fn dropped(&self) -> u32 {
        self.dropped
    }
    pub fn take_dropped(&mut self) -> u32 {
        core::mem::take(&mut self.dropped)
    }
    pub fn push(&mut self, b: u8) {
        if N == 0 {
            self.dropped = self.dropped.saturating_add(1);
            return;
        }
        if self.len == N {
            self.head = (self.head + 1) % N;
            self.len -= 1;
            self.dropped = self.dropped.saturating_add(1);
        }
        self.buf[(self.head + self.len) % N] = b;
        self.len += 1;
    }
    pub fn pop(&mut self) -> Option<u8> {
        if self.len == 0 {
            return None;
        }
        let b = self.buf[self.head];
        self.head = (self.head + 1) % N;
        self.len -= 1;
        Some(b)
    }
}
impl<const N: usize> Default for Ring<N> {
    fn default() -> Self {
        Self::new()
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
    fn keeps_the_newest_bytes_and_counts_the_loss() {
        let mut r = Ring::<4>::new();
        for b in 1..=6u8 {
            r.push(b);
        }
        assert_eq!(r.dropped(), 2);
        let out: Vec<u8> = core::iter::from_fn(|| r.pop()).collect();
        assert_eq!(out, [3, 4, 5, 6]);
        assert!(r.is_empty());
        assert_eq!(r.take_dropped(), 2);
        assert_eq!(r.dropped(), 0);
    }
    #[test]
    fn zero_sized_ring_drops_everything() {
        let mut r = Ring::<0>::new();
        r.push(1);
        assert_eq!((r.pop(), r.dropped()), (None, 1));
    }
    proptest! {
        /// Model check against a VecDeque with the same drop-oldest rule.
        #[test]
        fn matches_a_model(ops in proptest::collection::vec(proptest::option::of(any::<u8>()), 0..200)) {
            let mut r = Ring::<8>::new();
            let mut m: VecDeque<u8> = VecDeque::new();
            let mut lost = 0u32;
            for op in ops {
                match op {
                    Some(b) => {
                        if m.len() == 8 { m.pop_front(); lost += 1; }
                        m.push_back(b);
                        r.push(b);
                    }
                    None => prop_assert_eq!(r.pop(), m.pop_front()),
                }
                prop_assert_eq!(r.len(), m.len());
            }
            prop_assert_eq!(r.dropped(), lost);
        }
    }
}
