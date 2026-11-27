//! Verify an ESP image file with the core parser: `verify <image.bin>`. check.sh runs it on a real `esptool elf2image`
//! output of the stage, so the parser is pinned against the real packer and not only against the test builder.
//! Exit 0 and one `OK ...` line, or exit 1.
use boot_core::flash::{FlashError, FlashRead};
use std::process::ExitCode;

struct File(Vec<u8>);
impl FlashRead for File {
    fn read(&mut self, offset: u32, buf: &mut [u8]) -> Result<(), FlashError> {
        let o = offset as usize;
        buf.copy_from_slice(self.0.get(o..o + buf.len()).ok_or(FlashError)?);
        Ok(())
    }
}

fn main() -> ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: verify <image.bin>");
        return ExitCode::from(2);
    };
    let data = std::fs::read(&path).expect("read image");
    let len = data.len() as u32;
    match boot_core::image::verify(
        &mut File(data),
        0,
        len,
        boot_core::image::VerifyOpts { chip_id: boot_core::image::CHIP_ID_ESP32P4, require_hash: true },
        &mut || {},
    ) {
        Ok(v) => {
            let hex: String = v.sha256.iter().map(|b| format!("{b:02x}")).collect();
            println!(
                "OK len {} of {} segs {} entry 0x{:08x} sha256 {}",
                v.image_len, len, v.segment_count, v.entry, hex
            );
            ExitCode::SUCCESS
        }
        Err(e) => {
            println!("FAIL {e:?}");
            ExitCode::FAILURE
        }
    }
}
