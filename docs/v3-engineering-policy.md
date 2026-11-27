# Lichen OS v3: engineering policy (owner, 2026-11-27, #644)

v3 exists to be measurably better than v2 (targets: `v3/perf/targets.toml`, #643; roadmap #642), not to be a rewrite in Rust.
Apply this policy continuously. Don't open a study for each decision: timebox it, build, measure, change direction if needed.

## Priorities, in order
1. Security and hardware-enforced process isolation.
2. Fault containment and reliability.
3. Predictable performance under load.
4. Efficient memory and hardware resource use.
5. Responsiveness.
6. Boot speed.
7. Energy.
8. Maintainability and extensibility.

A faster OS that is less reliable or less secure is not an improvement. Security checks are never optimised away.

## Use the hardware we own (both HP cores, the C6, the bootloader)
- **Protection:** the kernel owns all PMP and PMA state and protects each process dynamically. A crash stays inside its process.
- **Memory:**
  - Critical kernel state lives in internal SRAM; PSRAM holds apps and bulk data.
  - Allocation is bounded with explicit owners, and DMA buffers come from fixed pools, so memory cannot fragment without limit.
  - Flash operations and cache coordination are centralised.
- **Scheduling:** scheduling is predictable and service queues are bounded, so no app or service can starve others. Cross-core IPC is efficient.
- **Boot:** the boot order puts early usability first, and noncritical services start asynchronously where that is safe.
- **Ownership and recovery:** the OS keeps hardware ownership. The P4 and the C6 recover independently where the hardware allows.

## v2 failure modes not to repeat
| v2 issue | Lesson |
|---|---|
| #121, #569 | PSRAM bit flips under DMA or while parked: pattern-test PSRAM, give DMA a fence, never trust it silently. |
| #260 | A malloc per frame on a DMA path gives an unbounded peak: use fixed buffer pools. |
| #471 | One long service call starved core 0 and fired the watchdog: bound the work per call and make it preemptible. |
| #530 | W^X was lost for apps in PSRAM: W^X for all native app memory is mandatory. |
| #593, #595 | Update payloads corrupted in staging and update paths tangled together: stream and verify end to end, use one install path, and always recover from a failed update. |

## Build ourselves (the architecture and security policy stay Lichen's)
- **Kernel and boot:** the microkernel, the boot stage and recovery policy, the scheduler, traps and interrupts, and context switching.
- **Isolation:** PMP and PMA management, process isolation, memory ownership and allocation policy.
- **Interfaces:** the syscall ABI, capabilities and IPC.
- **Processes:** process lifecycle and accounting, and app loading and execution policy.
- **Hardware and UI:** driver and hardware ownership, display composition and input routing policy.
- **System:** the P4 and C6 architecture and their trust boundary, update and package policy, fault containment and recovery.

Supporting libraries may be used inside these areas.

## Reuse instead of reinventing
- **Prefer existing Rust crates** for TCP/IP, TLS, crypto and signatures, filesystems, ELF parsing, serialisation, compression, fonts, image decoding, graphics primitives, time, common data structures, and test and benchmark tools.
- **Drivers:** port or adapt existing register-level drivers where practical.
- **C6 radio:** keep the existing C6 radio stacks where they are the most reliable option.
- **Our own instead:** a new in-house implementation must justify itself by a measured gain, easier maintenance, or an architectural need. A small piece of our own beats a large dependency.

## Dependency acceptance (a quick check, recorded in the commit or issue)
- **Licence:** MIT or Apache-2.0 only (`.claude/rules/licences.md`; `v3/tools/check_licences.py` enforces it).
- **Platform fit:**
  - It works with `no_std` where needed, on our RISC-V target.
  - Where it touches hardware, it works on **P4 revision 1.0**. Support for rev 3.x proves nothing about our chip.
  - It makes no hidden FreeRTOS, ESP-IDF or Linux assumptions.
- **Cost and quality:** size and RAM overhead are measured. It is maintained and documented, with few transitive dependencies and a small unsafe surface.
- **Build:** versions are pinned (`Cargo.lock` committed) and the build is reproducible.
- **Untrusted data:** a crate that parses untrusted input gets tests, a fuzz or proptest where cheap, and validation at our boundary.
- **Placement:** it sits behind a small interface so it can be replaced, and it never gives an app a route around the kernel boundary.

## Design preferences
Prefer:
- bounded memory over dynamic growth;
- explicit ownership over global state;
- process-local faults over a system panic;
- events over polling;
- zero-copy or low-copy IPC;
- streaming over full-image staging;
- small interfaces;
- measured over theoretical;
- simple over abstract.

## Portability
The kernel, platform support, drivers, services and apps stay independently understandable and replaceable.

Apps see the ABI and service contracts, never the implementation behind them; replacing an implementation keeps the ABI.

The WHY2025 badge is the first target, not a limit: board specifics live in the platform and board crates.
