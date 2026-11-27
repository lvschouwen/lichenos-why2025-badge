fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let plat = if std::env::var("CARGO_FEATURE_ESP32P4").is_ok() { "esp32p4" } else { "qemu-virt" };
    println!("cargo:rustc-link-arg=-T{dir}/link-{plat}.ld");
    println!("cargo:rerun-if-changed=link-{plat}.ld");
    println!("cargo:rerun-if-changed=src/boot_esp32p4.S");
    // The banner's build id comes from the build script (v3/build.sh: git sha, "-dirty" when the tree is not clean).
    println!("cargo:rerun-if-env-changed=LICHEN_BUILD_ID");
    let id = std::env::var("LICHEN_BUILD_ID").unwrap_or_else(|_| "unknown".into());
    println!("cargo:rustc-env=LICHEN_BUILD_ID={id}");
    // Only the qemu-virt build embeds apps; the bring-up image has none.
    if plat == "qemu-virt" {
        for v in ["LICHEN_APP_HEARTBEAT", "LICHEN_APP_ATTACKER"] {
            println!("cargo:rerun-if-env-changed={v}");
            if let Ok(p) = std::env::var(v) {
                println!("cargo:rerun-if-changed={p}");
            }
        }
    }
}
