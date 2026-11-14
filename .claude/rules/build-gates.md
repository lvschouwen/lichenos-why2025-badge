---
paths:
  - "tools/**"
  - "firmware/build.sh"
  - "host_tests/**"
---

# Build gates (moved from CLAUDE.md)

- Full check: `BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh build` = static checks (`tools/static_checks.sh`: shellcheck, cppcheck on changed C/C++ with `tools/cppcheck-baseline.txt`) + host tests (ctest, ASan+UBSan) + C6 image + storage staging + P4 image + layer check (`tools/check_layers.sh`, `tools/layers.conf` + shrink-only `tools/layers-baseline.txt`; the app boundary is its L6 profile via `tools/check_app_boundary.sh`) + licence check + the release build (#401: `firmware/build-release`, last, nested via `BADGE_RELEASE_NESTED` so static checks, host tests and the C6 image are not repeated; its marker check is `tools/check_release_image.sh`, which wants `LICHEN-DEBUG-BUILD-MARKER` absent from the release image and present in the developer image). `BADGE_SKIP_RELEASE=1` skips the release build and exits 3 so no green stamp is written. It must be green before a commit.
- ccache is on when installed (shared across worktree slots); `build.sh build` writes a gitignored `.clangd` for clangd/LSP.
- The standard gate `tools/check_standard.sh` runs inside `build` after the P4 image (lizard + `-fstack-usage` + P4/C6 image vs slot and per-archive size; fails when lizard is missing, `BADGE_SKIP_STANDARD=1` skips on purpose against `tools/standard-baseline.txt`; limits and ratchet rule in `docs/code-standard.md`). New files and functions past the fail line fail the build; the complexity baseline only shrinks. Archive sizes are per change against the parent: a green build rewrites the `size` lines of the baseline, commit them (never restore); >+8 KB per archive over the merge base with origin/main fails (rebuild and commit the size lines after a rebase), `--size-update` with a reason.
