//! A byte console with hand-rolled number output (no `core::fmt`: it costs several KB in a 24 KB stage).

pub trait Console {
    fn put(&mut self, b: u8);

    fn puts(&mut self, s: &str) {
        for b in s.bytes() {
            self.put(b);
        }
    }
    fn line(&mut self, s: &str) {
        self.puts(s);
        self.put(b'\n');
    }
    /// `0x` plus eight hex digits.
    fn hex32(&mut self, v: u32) {
        self.puts("0x");
        for i in (0..8).rev() {
            self.put(HEX[((v >> (i * 4)) & 0xF) as usize]);
        }
    }
    fn hex_bytes(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.put(HEX[(b >> 4) as usize]);
            self.put(HEX[(b & 0xF) as usize]);
        }
    }
    fn dec(&mut self, mut v: u32) {
        let mut buf = [0u8; 10];
        let mut i = buf.len();
        loop {
            i -= 1;
            buf[i] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
        for &b in &buf[i..] {
            self.put(b);
        }
    }
    /// A NUL- or 0xFF-padded name field, printable ASCII only.
    fn name(&mut self, n: &[u8]) {
        for &b in n {
            if b == 0 || b == 0xFF {
                break;
            }
            self.put(if (0x20..0x7F).contains(&b) { b } else { b'?' });
        }
    }
}

const HEX: &[u8; 16] = b"0123456789abcdef";

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    struct Buf(Vec<u8>);
    impl Console for Buf {
        fn put(&mut self, b: u8) {
            self.0.push(b);
        }
    }

    #[test]
    fn numbers() {
        let mut c = Buf(Vec::new());
        c.hex32(0x4FF2_8ED0);
        c.put(b' ');
        c.dec(0);
        c.put(b' ');
        c.dec(4_294_967_295);
        c.put(b' ');
        c.hex_bytes(&[0x0A, 0xFF]);
        c.put(b' ');
        c.name(b"ota_0\0\0\xFF");
        assert_eq!(core::str::from_utf8(&c.0).unwrap(), "0x4ff28ed0 0 4294967295 0aff ota_0");
    }
}
