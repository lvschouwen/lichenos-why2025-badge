# Stage entry (ROM hands over in M-mode, hart 0, interrupts off). Only t0 and t1 are used before stage_main, so a0
# (hart id) and a1 reach it unchanged. Hart 1, should it ever arrive, parks. A trap reports and parks.
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
2:  call stage_main
park:
    wfi
    j park

.section .text.trap, "ax"
.balign 256
.global trap_entry
trap_entry:
    la sp, _stack_top
    csrr a0, mcause
    csrr a1, mepc
    csrr a2, mtval
    call stage_trap
    j park
