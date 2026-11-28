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
    // Both builds embed the two harness apps, each linked for its platform's slots (v3/build.sh builds them first).
    // Without the variables (a plain cargo call, e.g. the gate's esp32p4 clippy) the images build.sh wrote are used.
    // The #618 process-stage apps run on the P4 only, so only that build needs them.
    let mut apps = vec![("LICHEN_APP_HEARTBEAT", "heartbeat"), ("LICHEN_APP_ATTACKER", "attacker")];
    if plat == "esp32p4" {
        apps.extend([("LICHEN_APP_WORKER", "worker"), ("LICHEN_APP_ROGUE", "rogue"), ("LICHEN_APP_ROGUE0", "rogue0")]);
    }
    for (v, app) in apps {
        println!("cargo:rerun-if-env-changed={v}");
        let p = std::env::var(v).unwrap_or_else(|_| format!("{dir}/../target/lichen/{plat}/{app}.bin"));
        if !std::path::Path::new(&p).is_file() {
            panic!("{v}: no app image at {p}; run v3/build.sh {plat} first");
        }
        println!("cargo:rerun-if-changed={p}");
        println!("cargo:rustc-env={v}={p}");
    }
}
