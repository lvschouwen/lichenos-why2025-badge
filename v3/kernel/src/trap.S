# Process trap entry and exit (#607 on qemu-virt, #613 on the ESP32-P4, #617 the process model on either core). Context
# = x0..x31 at 4*i, pc at 128, the mcycle of the trap entry at 132 (see trap.rs). Invariant: while a process runs,
# mscratch holds its Context; while the kernel runs, mscratch is 0. A trap taken in the kernel therefore finds 0 after
# the swap and goes to task_kernel_trap, never through a process's context. On the P4 this is a second vector next to
# boot_esp32p4.S's trap_entry (hence the names): core 0's harness installs it for good once the bring-up rows are done;
# core 1 installs it for the process stage and puts trap_entry back after task_leave (#617). mcause is never written
# (CLIC aliases mpp/mpie/mpil there).
.section .text.trap, "ax"
.balign 256
.global task_trap_entry
task_trap_entry:
    csrrw a0, mscratch, a0
    beqz a0, task_kernel_trap
    sw x1, 4(a0)
    sw x2, 8(a0)
    sw x3, 12(a0)
    sw x4, 16(a0)
    sw x5, 20(a0)
    csrr t0, mcycle             # x5 is saved: stamp the entry for the switch cost (kernel.ctx_switch)
    sw t0, 132(a0)
    sw x6, 24(a0)
    sw x7, 28(a0)
    sw x8, 32(a0)
    sw x9, 36(a0)
    sw x11, 44(a0)
    sw x12, 48(a0)
    sw x13, 52(a0)
    sw x14, 56(a0)
    sw x15, 60(a0)
    sw x16, 64(a0)
    sw x17, 68(a0)
    sw x18, 72(a0)
    sw x19, 76(a0)
    sw x20, 80(a0)
    sw x21, 84(a0)
    sw x22, 88(a0)
    sw x23, 92(a0)
    sw x24, 96(a0)
    sw x25, 100(a0)
    sw x26, 104(a0)
    sw x27, 108(a0)
    sw x28, 112(a0)
    sw x29, 116(a0)
    sw x30, 120(a0)
    sw x31, 124(a0)
    csrr t0, mscratch
    sw t0, 40(a0)
    csrr t0, mepc
    sw t0, 128(a0)
    csrw mscratch, zero
    # The kernel stack is the one task_run was called on, below its caller's frames (per hart: task_kctx).
    csrr t0, mhartid
    slli t0, t0, 6
    la t1, task_kctx
    add t1, t1, t0
    lw sp, 4(t1)
    call trap_handler
    j task_restore

# task_run(ctx) -> usize: save the caller's callee-saved registers, ra and sp in this hart's task_kctx slot, then enter
# ctx. mstatus.MPP must already be U. Returns only when the kernel calls task_leave(v), with v; until then every trap
# runs trap_handler on the saved sp, so the caller's frames stay intact.
.global task_run
task_run:
    csrr t0, mhartid
    slli t0, t0, 6
    la t1, task_kctx
    add t1, t1, t0
    sw ra, 0(t1)
    sw sp, 4(t1)
    sw s0, 8(t1)
    sw s1, 12(t1)
    sw s2, 16(t1)
    sw s3, 20(t1)
    sw s4, 24(t1)
    sw s5, 28(t1)
    sw s6, 32(t1)
    sw s7, 36(t1)
    sw s8, 40(t1)
    sw s9, 44(t1)
    sw s10, 48(t1)
    sw s11, 52(t1)
    sw gp, 56(t1)
    sw tp, 60(t1)
task_restore:
    lw t0, 128(a0)
    csrw mepc, t0
    csrw mscratch, a0
    lw x1, 4(a0)
    lw x2, 8(a0)
    lw x3, 12(a0)
    lw x4, 16(a0)
    lw x5, 20(a0)
    lw x6, 24(a0)
    lw x7, 28(a0)
    lw x8, 32(a0)
    lw x9, 36(a0)
    lw x11, 44(a0)
    lw x12, 48(a0)
    lw x13, 52(a0)
    lw x14, 56(a0)
    lw x15, 60(a0)
    lw x16, 64(a0)
    lw x17, 68(a0)
    lw x18, 72(a0)
    lw x19, 76(a0)
    lw x20, 80(a0)
    lw x21, 84(a0)
    lw x22, 88(a0)
    lw x23, 92(a0)
    lw x24, 96(a0)
    lw x25, 100(a0)
    lw x26, 104(a0)
    lw x27, 108(a0)
    lw x28, 112(a0)
    lw x29, 116(a0)
    lw x30, 120(a0)
    lw x31, 124(a0)
    lw x10, 40(x10)
    mret

# task_leave(v) -> !: from trap context (mscratch is 0), return v from this hart's task_run. The trap frames below the
# saved sp are dropped; the caller restores mtvec and mscratch for whatever runs next.
.global task_leave
task_leave:
    csrr t0, mhartid
    slli t0, t0, 6
    la t1, task_kctx
    add t1, t1, t0
    lw ra, 0(t1)
    lw sp, 4(t1)
    lw s0, 8(t1)
    lw s1, 12(t1)
    lw s2, 16(t1)
    lw s3, 20(t1)
    lw s4, 24(t1)
    lw s5, 28(t1)
    lw s6, 32(t1)
    lw s7, 36(t1)
    lw s8, 40(t1)
    lw s9, 44(t1)
    lw s10, 48(t1)
    lw s11, 52(t1)
    lw gp, 56(t1)
    lw tp, 60(t1)
    ret

# A trap in the kernel: the swap above left a0 = 0 and the kernel's a0 in mscratch. Swap back, so mscratch is 0 again
# while the kernel runs (any further trap also lands here), and report on a stack of its own, not over the frames of
# whatever trapped. A fault inside kernel_fault itself lands here again: the flag makes that one park in wfi instead
# of reporting (and faulting) over and over. Flag and fault stack are per hart (core 1 runs processes, #617).
task_kernel_trap:
    csrrw a0, mscratch, zero
    csrr t0, mhartid
    slli t2, t0, 2
    la t1, task_kfault_once
    add t1, t1, t2
    lw t2, 0(t1)
    bnez t2, 2f
    li t2, 1
    sw t2, 0(t1)
    bnez t0, 1f
    la sp, _kfault_stack_top
    j 3f
1:  la sp, _kfault_stack1_top
3:  call kernel_fault
2:  wfi
    j 2b

.section .bss
.balign 4
task_kfault_once: .space 8
# One 64-byte kernel context per hart (task_run / task_leave): ra, sp, s0..s11, gp, tp.
task_kctx: .space 128
