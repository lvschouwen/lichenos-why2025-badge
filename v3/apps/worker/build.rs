fn main() {
    // build.sh writes the slot's linker script and passes it here; without it this is a host build (clippy/tests).
    println!("cargo:rerun-if-env-changed=LICHEN_APP_LD");
    if let Ok(ld) = std::env::var("LICHEN_APP_LD") {
        println!("cargo:rerun-if-changed={ld}");
        println!("cargo:rustc-link-arg=-T{ld}");
    }
}
