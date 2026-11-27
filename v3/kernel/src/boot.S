# Entry: interrupts off, park every hart but 0 (the PoC is single-core), own stack, zero .bss, kmain.
.section .text.boot, "ax"
.global _start
_start:
    csrw mie, zero
    csrci mstatus, 8
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
