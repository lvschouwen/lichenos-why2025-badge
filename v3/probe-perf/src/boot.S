# #648 probe entry, the contract of probe-psram and the kernel: M-mode, interrupts off, mtvec set (CLIC mode, hardwired on the P4), own
# stack, mscratch = trap stack top, zero .bss, pmain. Hart 1 parks until core 0 starts it at _start1 (the dual-core suite).
# The trap vector keeps the interrupted sp in mscratch and runs on the hart's own trap stack: ptrap (main.rs) returns only for the
# CLINT timer interrupt of the irq suite; any other cause is a result and ends the run.
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
    la t0, _trap_stack_top
    csrw mscratch, t0
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

# Core 1 entry: core 0 hands this address to the ROM once core 1 is clocked and out of reset. Its own stacks; bss is core 0's.
.global _start1
.balign 4
_start1:
    csrci mstatus, 8
    la t0, trap_entry
    ori t0, t0, 3
    csrw mtvec, t0
    csrr t0, mhartid
    li t1, 1
    bne t0, t1, park
    la sp, _core1_stack_top
    la t0, _core1_trap_top
    csrw mscratch, t0
    call core1_main
    j park

.section .text.trap, "ax"
.balign 256
.global trap_entry
trap_entry:
    csrrw sp, mscratch, sp
    addi sp, sp, -64
    sw ra, 0(sp)
    sw t0, 4(sp)
    sw t1, 8(sp)
    sw t2, 12(sp)
    sw a0, 16(sp)
    sw a1, 20(sp)
    sw a2, 24(sp)
    sw a3, 28(sp)
    sw a4, 32(sp)
    sw a5, 36(sp)
    sw a6, 40(sp)
    sw a7, 44(sp)
    sw t3, 48(sp)
    sw t4, 52(sp)
    sw t5, 56(sp)
    sw t6, 60(sp)
    csrr a0, mcause
    call ptrap
    lw ra, 0(sp)
    lw t0, 4(sp)
    lw t1, 8(sp)
    lw t2, 12(sp)
    lw a0, 16(sp)
    lw a1, 20(sp)
    lw a2, 24(sp)
    lw a3, 28(sp)
    lw a4, 32(sp)
    lw a5, 36(sp)
    lw a6, 40(sp)
    lw a7, 44(sp)
    lw t3, 48(sp)
    lw t4, 52(sp)
    lw t5, 56(sp)
    lw t6, 60(sp)
    addi sp, sp, 64
    csrrw sp, mscratch, sp
    mret
