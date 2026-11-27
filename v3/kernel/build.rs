fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    let plat = if std::env::var("CARGO_FEATURE_ESP32P4").is_ok() { "esp32p4" } else { "qemu-virt" };
    println!("cargo:rustc-link-arg=-T{dir}/link-{plat}.ld");
    println!("cargo:rerun-if-changed=link-{plat}.ld");
    for v in ["LICHEN_APP_HEARTBEAT", "LICHEN_APP_ATTACKER"] {
        println!("cargo:rerun-if-env-changed={v}");
        if let Ok(p) = std::env::var(v) {
            println!("cargo:rerun-if-changed={p}");
        }
    }
}
