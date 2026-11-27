fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo:rustc-link-arg=-T{dir}/link.ld");
    println!("cargo:rerun-if-changed=link.ld");
    // The stage is baked in at build time (build.sh) so one image does one job, as the #609 suites do.
    println!("cargo:rerun-if-env-changed=LICHEN_PSRAM_STAGE");
    let stage = std::env::var("LICHEN_PSRAM_STAGE").unwrap_or_else(|_| "all".into());
    println!("cargo:rustc-env=LICHEN_PSRAM_STAGE={stage}");
}
