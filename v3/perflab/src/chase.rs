//! Pointer-chase permutations. The latency test walks a ring of slots that visits every slot exactly once in a random order, so
//! neither the prefetcher nor the cache sets can guess the next address and the loop is a chain of dependent loads.
use crate::rng::Rng;

/// Fill `perm` so that following `i -> perm[i]` from any slot visits all slots once (Sattolo's shuffle: one cycle).
pub fn sattolo(perm: &mut [u32], rng: &mut Rng) {
    for (i, p) in perm.iter_mut().enumerate() {
        *p = i as u32;
    }
    for i in (1..perm.len()).rev() {
        let j = rng.below(i as u32) as usize; // 0..i, never i itself: that is what keeps it a single cycle
        perm.swap(i, j);
    }
}

/// Length of the cycle that starts at `start` (a ring of all slots has length `perm.len()`).
pub fn cycle_len(perm: &[u32], start: u32) -> usize {
    let (mut n, mut at) = (1, perm[start as usize]);
    while at != start {
        at = perm[at as usize];
        n += 1;
        if n > perm.len() {
            return 0; // not a cycle through `start`
        }
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec;

    #[test]
    fn one_cycle_over_every_slot() {
        for n in [1usize, 2, 3, 17, 256, 4096] {
            let mut p = vec![0u32; n];
            sattolo(&mut p, &mut Rng::new(n as u32 + 1));
            assert_eq!(cycle_len(&p, 0), n, "n={n}");
            // no slot points at itself (for n > 1): every hop moves
            if n > 1 {
                assert!(p.iter().enumerate().all(|(i, &v)| v as usize != i));
            }
        }
    }

    #[test]
    fn different_seeds_give_different_rings() {
        let (mut a, mut b) = (vec![0u32; 64], vec![0u32; 64]);
        sattolo(&mut a, &mut Rng::new(1));
        sattolo(&mut b, &mut Rng::new(2));
        assert_ne!(a, b);
    }
}
