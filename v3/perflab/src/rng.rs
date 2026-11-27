//! A small deterministic generator for the random-access and pointer-chase tests (the same run is the same run).

#[derive(Clone, Copy)]
pub struct Rng(u32);
impl Rng {
    /// A zero seed would stick at zero, so it is replaced.
    pub const fn new(seed: u32) -> Self {
        Rng(if seed == 0 { 0x9E37_79B9 } else { seed })
    }
    pub fn next_u32(&mut self) -> u32 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.0 = x;
        x
    }
    /// A value in `0..n` (multiply-shift, no division in the timed loop). `n` of 0 gives 0.
    pub fn below(&mut self, n: u32) -> u32 {
        ((self.next_u32() as u64 * n as u64) >> 32) as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn deterministic_and_never_stuck() {
        let (mut a, mut b) = (Rng::new(7), Rng::new(7));
        for _ in 0..100 {
            assert_eq!(a.next_u32(), b.next_u32());
        }
        let mut z = Rng::new(0);
        assert_ne!(z.next_u32(), 0);
    }

    #[test]
    fn below_stays_in_range_and_covers_it() {
        let mut r = Rng::new(1);
        let mut seen = [false; 16];
        for _ in 0..2000 {
            let v = r.below(16);
            assert!(v < 16);
            seen[v as usize] = true;
        }
        assert!(seen.iter().all(|&s| s));
        assert_eq!(r.below(0), 0);
        assert_eq!(r.below(1), 0);
    }
}
