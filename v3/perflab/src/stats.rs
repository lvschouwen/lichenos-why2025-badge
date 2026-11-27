//! Summary statistics of a sample set, on top of `kcore::perf` (nearest-rank percentiles).
use lichen_kcore::perf::median_p95;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Summary {
    pub min: u32,
    pub median: u32,
    pub p95: u32,
    pub max: u32,
}

/// Sorts `v` in place. `None` for an empty set.
pub fn summarize(v: &mut [u32]) -> Option<Summary> {
    let (median, p95) = median_p95(v)?;
    Some(Summary { min: v[0], median, p95, max: v[v.len() - 1] })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn summary_of_samples() {
        assert_eq!(summarize(&mut []), None);
        assert_eq!(summarize(&mut [9]), Some(Summary { min: 9, median: 9, p95: 9, max: 9 }));
        let mut v: [u32; 100] = core::array::from_fn(|i| (100 - i) as u32);
        assert_eq!(summarize(&mut v), Some(Summary { min: 1, median: 51, p95: 95, max: 100 }));
    }
}
