# Lichen v3 feasibility PoC (#607, epic #603)

An experiment, not the OS. It answers one question: can native apps run in RISC-V U-mode, behind per-app PMP,
under a small Rust kernel that survives every misbehaving app? It is kept off the v2 firmware build.

- `abi/`: the app ABI v1, numbers and registers only (callable from C), plus the app runtime (`rt` feature).
- `kernel/`: M-mode kernel: boot, trap entry/exit, round-robin scheduler with CPU budgets, PMP per app
  (text R+X, data R+W, everything else denied), syscalls with range-checked copy-in, generation-checked handles,
  image validation, and the isolation harness (`tests.rs`).
- `apps/heartbeat` (B) and `apps/attacker` (A): compiled independently, each linked for its own slot
  (`apps/app.ld.in`), and embedded as images that the kernel validates before loading.

```
./build.sh qemu-virt run      # needs qemu-system-riscv32 (Ubuntu: qemu-system-riscv)
```

## Status
| Where | Result | What it proves |
|---|---|---|
| QEMU virt (rv32, 16 PMP, G=4 B) | 21/21 pass, B alive after every case | the kernel logic and the PMP model per the RISC-V spec |
| ESP32-P4 rev v1.0 | not run | nothing yet: the silicon question is open (#604) |

Not covered: DMA masters, the second HP core, the LP core, cache/MMU aliases, interrupts other than the timer.
An emulator result is never evidence of hardware isolation.
