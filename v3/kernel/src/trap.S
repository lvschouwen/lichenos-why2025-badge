# Task trap entry and exit for the isolation harness (#607 on qemu-virt, #613 on the ESP32-P4). Context = x0..x31 at
# 4*i, pc at 128 (see trap.rs). Invariant: while an app runs, mscratch holds its Context; while the kernel runs, mscratch
# is 0. A trap taken in the kernel therefore finds 0 after the swap and goes to task_kernel_trap, never through an app's
# context. On the P4 this is a second vector next to boot_esp32p4.S's trap_entry (hence the names): the harness installs
# it once the bring-up rows are done and never goes back. mcause is never written (CLIC aliases mpp/mpie/mpil there).
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
    la sp, _stack_top
    call trap_handler
    j task_restore

# enter_user(ctx): mstatus.MPP must already be U.
.global enter_user
enter_user:
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

# A trap in the kernel: the swap above left a0 = 0 and the kernel's a0 in mscratch. Swap back, so mscratch is 0 again
# while the kernel runs (any further trap also lands here), and report on a stack of its own, not over the frames of
# whatever trapped. A fault inside kernel_fault itself lands here again: the flag makes that one park in wfi instead
# of reporting (and faulting) over and over.
task_kernel_trap:
    csrrw a0, mscratch, zero
    la t0, task_kfault_once
    lw t1, 0(t0)
    bnez t1, 2f
    li t1, 1
    sw t1, 0(t0)
    la sp, _kfault_stack_top
    call kernel_fault
2:  wfi
    j 2b

.section .bss
.balign 4
task_kfault_once: .word 0
