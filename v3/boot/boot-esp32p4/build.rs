// The stage lives where the ROM loads the second stage: 0x4FF28ED0..0x4FF3ABD0 (#615). The RAM proof can be linked
// lower (LICHEN_BOOT_BASE=0x4FF10000) when the ROM download mode does not leave that window alone; the length is
// the same either way. The region holds the loaded image, then bss and the stack.
fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let out = std::env::var("OUT_DIR").unwrap();
    println!("cargo:rerun-if-env-changed=LICHEN_BOOT_BASE");
    println!("cargo:rerun-if-changed=link.ld");
    println!("cargo:rerun-if-changed=src/boot.S");
    let base = std::env::var("LICHEN_BOOT_BASE").unwrap_or_else(|_| "0x4FF28ED0".into());
    let base = u32::from_str_radix(base.trim_start_matches("0x"), 16).expect("LICHEN_BOOT_BASE is hex");
    assert!(
        base.is_multiple_of(16) && (0x4FF1_0000..=0x4FF2_8ED0).contains(&base),
        "LICHEN_BOOT_BASE outside 0x4FF10000..0x4FF28ED0"
    );
    std::fs::write(format!("{out}/region.ld"), format!("STAGE_BASE = {base:#x};\nSTAGE_LEN = 0x11D00;\n")).unwrap();
    println!("cargo:rustc-link-search={out}");
    println!("cargo:rustc-link-arg=-T{dir}/link.ld");
}
