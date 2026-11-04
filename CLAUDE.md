# Lichen OS (WHY2025 badge)

Lichen OS is the operating system for the WHY2025 badge: the ESP32-P4 runs Lichen (ESP-IDF 6.1, LVGL 9.6), the ESP32-C6 is a radio coprocessor (Wi-Fi via esp_hosted, LoRa). MeshCore is an app on top. Spelling: Lichen, MeshCore.

## Layout
- `firmware/` P4 image (components under `firmware/components/`, apps behind `lichen_api`); `c6/` C6 image; `shared/` code built into both (`shared/lora`).
- `host_tests/` ESP-free unit tests of pure logic; `tools/` badge.sh, licence and boundary checks, asset packers; `docs/` design docs.
- Submodules: `meshcore/` (fork, branch why2025-badge), `esp_hosted/` (fork), `third_party/*`.

## Build and check
- Full check: `BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh build` = host tests (ctest) + C6 image + storage staging + P4 image + app boundary check (`tools/check_app_boundary.sh`) + licence check. It must be green before a commit.
- Host tests only: `cmake -S host_tests -B host_tests/build && cmake --build host_tests/build -j8 && ctest --test-dir host_tests/build`.
- After committing run `./firmware/build.sh reconfigure build` so the image label `fw-<sha>` matches the commit.
- The build reformats `dependencies.lock`: `git restore dependencies.lock` unless dependencies really changed.
- `sdkconfig` is gitignored; `sdkconfig.defaults` (firmware/ and c6/) is the truth. Run long builds to a log file with a 600 s timeout.
- A behaviour change ships with a host test that fails before and passes after; put pure logic in ESP-free `.c` files.

## Licences
- `c6/` and `shared/lora/` are Apache-2.0 (the C6 image links Espressif blobs): no GPL or BadgeVMS code there.
- A new or newly enabled library gets an entry in `tools/third_party_licenses.py`; regenerate `THIRD_PARTY_LICENSES.txt`.

## Docs
- `docs/architecture.md` (tasks, memory, boot), `docs/app-api.md` (what apps may call), `docs/shell-design.md` (UI shell, screens, navigation).

## Hardware (main session only)
- `tools/badge.sh flash-fw <port>` keeps `/storage` (MeshCore identity, contacts, channels) by default. `--wipe-storage` is first-install only and only Lucas decides it (a hook asks).
- Wait 75 s after any reboot before console commands. Bench procedure: the `lichen-bench` skill.
- No IPs, MACs, host names, SSIDs or tokens in code, commits or issues.

## Agents
- Subagents are build-only: never flash, ssh, push or touch the badge; stop at a green `build.sh build` + a local commit. Agent file: `.claude/agents/lichen-implementer.md`; reviewer: `lichen-reviewer.md`.
- Pass `model` on every dispatch: Opus for concurrency, memory, protocol, crypto/auth or hardware-adjacent work, Sonnet otherwise.
- Worktree pool: agents work in `.claude/worktrees/*`. Never `git checkout`/`switch`/`reset` in the main checkout (a hook asks); main merges one branch at a time, ff-only, then runs the full check.

## Issues and memory
- The issue is the spec: `gh issue view N --json body,comments --jq '.body, .comments[].body'` (plain `--comments` is broken here); the latest decision comment wins.
- State lives in issues (labels `gain:N`/`ease:N`/`score:N`, `needs-lucas` for bench checks waiting on Lucas). Memory holds standing knowledge only.
- graphify: local AST graph of `firmware/` only (gitignored); use `graphify query "<q>"` for cross-file questions, `graphify update .` after merges.
