//! Generation-checked handles: value = gen << 8 | idx. A handle is valid only for its owner and only until closed;
//! closing or the owner's death bumps the generation, so stale copies and copies stolen from another app fail.
use lichen_abi::err;

pub const SLOTS: usize = 8;
pub const PER_APP: usize = 4;

#[derive(Clone, Copy)]
struct Entry {
    owner: Option<usize>,
    gen: u16,
}
pub struct Handles {
    e: [Entry; SLOTS],
}
impl Handles {
    pub const fn new() -> Self {
        Self { e: [Entry { owner: None, gen: 1 }; SLOTS] }
    }
    pub fn open(&mut self, owner: usize) -> isize {
        if self.e.iter().filter(|e| e.owner == Some(owner)).count() >= PER_APP {
            return err::EMFILE;
        }
        match self.e.iter().position(|e| e.owner.is_none()) {
            Some(i) => {
                self.e[i].owner = Some(owner);
                ((self.e[i].gen as usize) << 8 | i) as isize
            }
            None => err::EMFILE,
        }
    }
    fn lookup(&self, owner: usize, h: usize) -> Option<usize> {
        let (idx, gen) = (h & 0xFF, h >> 8);
        (idx < SLOTS && gen <= 0xFFFF && self.e[idx].owner == Some(owner) && self.e[idx].gen as usize == gen)
            .then_some(idx)
    }
    pub fn check(&self, owner: usize, h: usize) -> isize {
        if self.lookup(owner, h).is_some() {
            0
        } else {
            err::EBADF
        }
    }
    pub fn close(&mut self, owner: usize, h: usize) -> isize {
        match self.lookup(owner, h) {
            Some(i) => {
                self.e[i] = Entry { owner: None, gen: self.e[i].gen.wrapping_add(1).max(1) };
                0
            }
            None => err::EBADF,
        }
    }
    pub fn release_owner(&mut self, owner: usize) {
        for e in self.e.iter_mut().filter(|e| e.owner == Some(owner)) {
            *e = Entry { owner: None, gen: e.gen.wrapping_add(1).max(1) };
        }
    }
    pub fn first_of(&self, owner: usize) -> Option<usize> {
        self.e.iter().position(|e| e.owner == Some(owner)).map(|i| (self.e[i].gen as usize) << 8 | i)
    }
}

impl Default for Handles {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_check_close() {
        let mut h = Handles::new();
        let a = h.open(0) as usize;
        assert_eq!(h.check(0, a), 0);
        assert_eq!(h.check(1, a), err::EBADF, "another owner cannot use it");
        assert_eq!(h.close(0, a), 0);
        assert_eq!(h.check(0, a), err::EBADF, "stale after close");
        assert_eq!(h.close(0, a), err::EBADF);
    }
    #[test]
    fn per_app_and_global_limits() {
        let mut h = Handles::new();
        for _ in 0..PER_APP {
            assert!(h.open(0) >= 0);
        }
        assert_eq!(h.open(0), err::EMFILE);
        for _ in 0..(SLOTS - PER_APP) {
            assert!(h.open(1) >= 0);
        }
        assert_eq!(h.open(2), err::EMFILE);
    }
    #[test]
    fn release_owner_invalidates_copies() {
        let mut h = Handles::new();
        let a = h.open(3) as usize;
        h.release_owner(3);
        assert_eq!(h.check(3, a), err::EBADF);
        assert_eq!(h.first_of(3), None);
    }
    #[test]
    fn forged_values_fail() {
        let h = Handles::new();
        for v in [0usize, 0xFF, 0x0100_0000, usize::MAX] {
            assert_eq!(h.check(0, v), err::EBADF);
        }
    }
}
