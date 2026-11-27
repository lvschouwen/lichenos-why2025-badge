# Entry contract (load-ram and stage-booted alike): M-mode, interrupts off, a0 = hart id, a1 = boot-info pointer or NULL,
# no stack assumed. This code only uses t0 and t1 before kmain, so a0 and a1 reach it unchanged.
# ESP32-P4 entry, reached from the ROM download mode on hart 0. Interrupts off, mtvec set (CLIC mode, as the P4 hardwires
# it), own stack, mscratch = trap stack top, zero .bss, kmain. Hart 1 (if it ever lands here) parks.
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
2:  call kmain
park:
    wfi
    j park

.section .text.trap, "ax"
# M-mode trap vector (#612). CLIC mode: exceptions and non-vectored interrupts land at the mtvec base (64-byte aligned
# at least). Invariant: outside a trap, mscratch holds this hart's trap stack top; inside one, it is 0. So a trap taken
# while a trap is being handled swaps in 0, and goes to the nested-trap path on its own stack instead of overwriting
# the frame. Only caller-saved registers are saved: m_trap is a normal C-ABI function and keeps the rest.
# mcause is never written (CLIC: it aliases mpp/mpie and carries mpil, which mret restores).
.balign 256
.global trap_entry
trap_entry:
    csrrw sp, mscratch, sp
    beqz sp, nested_trap
    addi sp, sp, -80
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
    csrr t0, mscratch
    sw t0, 64(sp)
    csrw mscratch, zero
    mv a0, sp
    call m_trap
    lw t0, 64(sp)
    csrw mscratch, t0
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
    addi sp, sp, 80
    csrrw sp, mscratch, sp
    mret

nested_trap:
    csrw mscratch, zero
    la sp, _fault_stack_top
    call nested_fault
    j park

# planted_fault(): one illegal instruction (the all-zero word is defined illegal), then return. The bring-up arms
# mtrap::expect_fault with this address first, so the handler dumps it and skips the 4-byte word.
.section .text, "ax"
.global planted_fault
.balign 4
planted_fault:
    .word 0
    ret
