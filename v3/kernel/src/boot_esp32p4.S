# Entry contract (load-ram and stage-booted alike): M-mode, interrupts off, a0 = hart id, a1 = boot-info pointer or NULL,
# no stack assumed. This code only uses t0 and t1 before kmain, so a0 and a1 reach it unchanged.
# ESP32-P4 entry, reached from the ROM download mode on hart 0. Interrupts off, mtvec set (CLIC mode, as the P4 hardwires
# it), own stack, zero .bss, kmain. Hart 1 (if it ever lands here) parks. A trap lands in trap_entry on its own stack and
# only reports (fatal_trap); real trap handling is #612.
.option norvc

.section .text.boot, "ax"
.global _start
_start:
    csrci mstatus, 8
    la t0, trap_entry
    ori t0, t0, 3
    csrw mtvec, t0
    csrr t0, mhartid
    bnez t0, park
    la sp, _stack_top
    la t0, _sbss
    la t1, _ebss
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  call kmain
park:
    wfi
    j park

.section .text.trap, "ax"
.balign 256
.global trap_entry
trap_entry:
    la sp, _trap_stack_top
    call fatal_trap
    j park
