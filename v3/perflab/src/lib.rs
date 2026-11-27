//! #648 perf lab: everything of the benchmark harness that is not a register access. The probe (`probe-perf`) owns the hardware;
//! this crate owns the numbers it prints and the words it writes, so both are tested on the host.
#![no_std]
#[cfg(test)]
extern crate std;

pub mod cfg;
pub mod chase;
pub mod dma;
pub mod fmt;
pub mod l2;
pub mod ldo;
pub mod line;
pub mod patterns;
pub mod rate;
pub mod rng;
pub mod stats;
pub mod temp;
