# #620 probe entry, reached from the ROM download mode on hart 0 (same contract as kernel/src/boot_esp32p4.S): M-mode,
# interrupts off, mtvec set (CLIC mode, hardwired on the P4), own stack, zero .bss, pmain. Hart 1 parks. A trap reports mcause, mepc and
# mtval from ptrap (main.rs) and parks.
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
2:  call pmain
park:
    wfi
    j park

.section .text.trap, "ax"
.balign 256
.global trap_entry
trap_entry:
    la sp, _trap_stack_top
    call ptrap
    j park
