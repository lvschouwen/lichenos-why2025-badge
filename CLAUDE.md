# Lichen OS (WHY2025 badge)
ESP32-P4 runs Lichen (ESP-IDF 6.1, LVGL 9.6); ESP32-C6 is a radio coprocessor (Wi-Fi via esp_hosted, LoRa). MeshCore is an app on top. Spelling: Lichen, MeshCore.

## Layout
- `firmware/` P4 image (`firmware/components/`, apps behind `lichen_api`); `c6/` C6 image; `shared/lora` built into both; `host_tests/` ESP-free unit tests; `tools/` badge.sh and checks; `docs/` design docs.
- Submodules: `meshcore/` (fork, branch why2025-badge; never write in it), `esp_hosted/` (fork), `third_party/*`.

## Build and check
- Full check, green before every commit: `BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh build` (static checks, host tests under ASan+UBSan, C6 + P4 images, layer, standard and licence checks). Log to a file, 600 s timeout. Gate details: `.claude/rules/build-gates.md`.
- Host tests only: `cmake -S host_tests -B host_tests/build && cmake --build host_tests/build -j8 && ctest --test-dir host_tests/build`.
- The full check ends with the release build (`firmware/build-release`, +~1.5 min warm, #401); `BADGE_SKIP_RELEASE=1` skips it but exits 3, so it never stamps green.
- A green build rewrites the `size` lines of `tools/standard-baseline.txt`: commit them, never restore. More than +8 KB per archive over the merge base fails; baselines only shrink.
- `git restore dependencies.lock` unless dependencies changed. After committing: `BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh reconfigure build` (label `fw-<sha>`).
- `sdkconfig` is gitignored; `sdkconfig.defaults` (firmware/ and c6/) is the truth.
- A behaviour change ships with a host test that fails before and passes after; pure logic goes in ESP-free `.c` files.

## Never
- GPL or BadgeVMS code in `c6/` or `shared/lora/` (Apache-2.0). New libraries: `.claude/rules/licences.md`.
- IPs, MACs, host names, SSIDs or tokens in code, commits or issues.
- Flash, console, OTA outside the main session (`lichen-bench` skill). `--wipe-storage` only when Lucas decides. Wait 75 s after a reboot.

## Where to look
- `docs/architecture.md`, `docs/app-api.md`, `docs/shell-design.md`, `docs/code-standard.md`, `docs/ui-standard.md` (12 UI rules; cite numbers in reviews).
- Code questions: `graphify query "<q>"` first; `graphify update` (no path) after merges. More: `.claude/docs/graphify.md`.
- The issue is the spec: `gh issue view N --json body,comments --jq '.body, .comments[].body'`; the latest decision wins. Labels, scoring, bench checks: `.claude/docs/issues.md`.

## Agents (standard: `~/.claude/rules/common/agent-runs.md`)
- Check `agent-check` (`reconfigure` relabels); warm pool `agent-wt`; guards `lichen-implementer.md`, `lichen-reviewer.md`, `.claude/hooks/guard-badge.sh`.
- Opus triggers: the LVGL lock / UI service, task and memory books, raw-hook tables, the P4 <-> C6 contract (#234), DMA/internal RAM, anything that changes what the badge transmits.
- Conflict files: `host_tests/CMakeLists.txt`, component `CMakeLists.txt`, `tools/layers.conf`, `tools/layers-baseline.txt`, `tools/standard-baseline.txt`, `tools/cppcheck-baseline.txt`, `tools/gate_manifest.txt`, `sdkconfig.defaults`, `boot_order[]`.
