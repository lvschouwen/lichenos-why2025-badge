//! L2 cache options of the matrix: size 128, 256 or 512 KB, line 64 or 128 B, as IDF's `Cache_Set_L2_Cache_Mode(size, 8, line)`
//! (the ways argument is the literal 8 in IDF, although the ROM enum says 2: the probe passes what IDF passes).
use lichen_kcore::l2::{carve_start, L2MEM_END};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct L2Cfg {
    pub kb: u32,
    pub line: u32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Refuse {
    Size,
    Line,
    /// IDF forces 128 B lines for 512 KB (the tag RAM has no room for 64 B).
    Line512,
}
impl L2Cfg {
    pub const fn check(&self) -> Result<(), Refuse> {
        if !(self.kb == 128 || self.kb == 256 || self.kb == 512) {
            return Err(Refuse::Size);
        }
        if !(self.line == 64 || self.line == 128) {
            return Err(Refuse::Line);
        }
        if self.kb == 512 && self.line != 128 {
            return Err(Refuse::Line512);
        }
        Ok(())
    }
    /// `cache_size_t`: CACHE_SIZE_128K = 9, 256K = 10, 512K = 11.
    pub const fn size_code(&self) -> u32 {
        match self.kb {
            128 => 9,
            256 => 10,
            _ => 11,
        }
    }
    /// `cache_line_size_t`: 64 B = 3, 128 B = 4.
    pub const fn line_code(&self) -> u32 {
        if self.line == 128 {
            4
        } else {
            3
        }
    }
    /// First address the cache takes from L2MEM.
    pub const fn carve(&self) -> usize {
        carve_start(self.kb as usize * 1024)
    }
    /// Does NOLOAD memory ending at `noload_end` stay below the carve?
    pub const fn fits_below(&self, noload_end: usize) -> bool {
        noload_end <= self.carve() && noload_end <= L2MEM_END
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn c(kb: u32, line: u32) -> L2Cfg {
        L2Cfg { kb, line }
    }

    #[test]
    fn the_ids_idf_and_v2_use() {
        // v2: 256 KB, 128 B = (10, 8, 4); the ROM state is 256 KB / 64 B
        assert_eq!((c(256, 128).size_code(), c(256, 128).line_code()), (10, 4));
        assert_eq!((c(256, 64).size_code(), c(256, 64).line_code()), (10, 3));
        assert_eq!((c(128, 64).size_code(), c(512, 128).size_code()), (9, 11));
    }

    #[test]
    fn allowed_combinations() {
        for (kb, line, ok) in [
            (128, 64, true),
            (128, 128, true),
            (256, 64, true),
            (256, 128, true),
            (512, 128, true),
            (512, 64, false),
            (64, 64, false),
            (1024, 128, false),
            (256, 32, false),
        ] {
            assert_eq!(c(kb, line).check().is_ok(), ok, "{kb} KB {line} B");
        }
        assert_eq!(c(512, 64).check(), Err(Refuse::Line512));
    }

    #[test]
    fn the_carve_and_the_probe_noload_window() {
        assert_eq!(c(256, 64).carve(), 0x4FF8_0000);
        assert_eq!(c(512, 128).carve(), 0x4FF4_0000);
        assert_eq!(c(128, 64).carve(), 0x4FFA_0000);
        // probe-perf keeps bss, stacks and buffers in 0x4FF2_9000..0x4FF3_A000 so every size fits, 512 KB included
        for kb in [128, 256, 512] {
            assert!(c(kb, 128).fits_below(0x4FF3_A000), "{kb} KB");
        }
        // probe-psram's map (NOLOAD to 0x4FF8_0000) does not fit a 512 KB carve
        assert!(!c(512, 128).fits_below(0x4FF8_0000));
    }
}
