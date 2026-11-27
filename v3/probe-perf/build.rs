//! Link script for the binary only (a link-arg for the host test builds of the workspace would break them), and the build-time
//! options of the matrix (see build.sh): they reach the code as `env!` strings and are validated in `main.rs` at compile time.
fn main() {
    let dir = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo:rustc-link-arg-bins=-T{dir}/link.ld");
    println!("cargo:rerun-if-changed=link.ld");
    for (var, default) in [
        ("LICHEN_PERF_CPU", "360"),
        ("LICHEN_PERF_PSRAM", "120"),
        ("LICHEN_PERF_L2KB", "256"),
        ("LICHEN_PERF_L2LINE", "64"),
        ("LICHEN_PERF_DMA", "32"),
        ("LICHEN_PERF_LDO", "cal"),
        ("LICHEN_PERF_SUITE", "smoke"),
        ("LICHEN_PERF_SECS", "0"),
        ("LICHEN_PERF_ID", "dev"),
    ] {
        println!("cargo:rerun-if-env-changed={var}");
        println!("cargo:rustc-env={var}={}", std::env::var(var).unwrap_or_else(|_| default.into()));
    }
}
