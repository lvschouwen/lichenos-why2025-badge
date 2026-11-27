fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo::rustc-check-cfg=cfg(probe_app)");
    if std::env::var("LICHEN_PROBE_RUN").as_deref() == Ok("app") {
        // #608 step 3b: an app image for the bootloader, with its esp_app_desc_t (app_desc.rs)
        println!("cargo:rustc-cfg=probe_app");
        println!("cargo:rustc-link-arg=-L{dir}");
        println!("cargo:rustc-link-arg=-T{dir}/link-app.ld");
    } else {
        println!("cargo:rustc-link-arg=-T{dir}/link.ld");
    }
    println!("cargo:rerun-if-changed=link.ld");
    println!("cargo:rerun-if-changed=link-app.ld");
    println!("cargo:rerun-if-changed=src/asm.S");
    println!("cargo:rerun-if-env-changed=LICHEN_PROBE_RUN");
    println!("cargo:rerun-if-env-changed=LICHEN_PROBE_SUITE");
}
