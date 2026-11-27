# Entry contract (load-ram and stage-booted alike): M-mode, interrupts off, a0 = hart id, a1 = boot-info pointer or NULL,
# no stack assumed. This code only uses t0 and t1 before kmain, so a0 and a1 reach it unchanged.
# ESP32-P4 entry, reached from the ROM download mode on hart 0. Interrupts off, mtvec set (CLIC mode, as the P4 hardwires
# it), own stack, mscratch = trap stack top, zero .bss, kmain. Hart 1 (if it ever lands here) parks.
.option norvc

.section .text.boot, "ax"
.global _start
_start:
    csrci mstatus, 8
    li t0, 0x6000               # mstatus.FS = off: no FPU state for U to see or leave behind
    csrc mstatus, t0
    li t0, 0x200000             # mstatus.TW = 1 requested; the bench shows it reads back 0 (WARL), so U can run wfi
    csrs mstatus, t0            # and only a running tick wakes it (run_u always has one)
    la t0, trap_entry
    ori t0, t0, 3
    csrw mtvec, t0
    csrr t0, mhartid
    bnez t0, park
    # U-visible state starts at zero: HWLP and PIE state OFF, no U access to the cycle counters.
    csrw 0x7f1, zero
    csrw 0x7f2, zero
    csrw 0x306, zero
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
# the frame. The window between the swap and the `csrrw t0, mscratch, zero` is one addi and one store to the trap
# stack (kernel RAM, M-mode, unlocked PMP): neither can fault, and interrupts cannot be taken (MIE is cleared on entry).
# Only caller-saved registers are saved: m_trap is a normal C-ABI function and keeps the rest.
# mcause is never written (CLIC: it aliases mpp/mpie and carries mpil, which mret restores).
.balign 256
.global trap_entry
trap_entry:
    csrrw sp, mscratch, sp
    beqz sp, nested_trap
    addi sp, sp, -80
    sw t0, 4(sp)
    csrrw t0, mscratch, zero    # t0 = interrupted sp, mscratch = 0 in one instruction
    sw t0, 64(sp)
    sw ra, 0(sp)
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
    mv a0, sp
    call m_trap
    bnez a0, to_kernel
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

# m_trap returned nonzero: the U run is over. Drop the U frame, re-arm mscratch with the trap stack top, and return
# from run_user into the kernel with its saved callee-saved registers.
to_kernel:
    addi t0, sp, 80
    csrw mscratch, t0
    la t0, k_ctx
    lw ra, 0(t0)
    lw sp, 4(t0)
    lw s0, 8(t0)
    lw s1, 12(t0)
    lw s2, 16(t0)
    lw s3, 20(t0)
    lw s4, 24(t0)
    lw s5, 28(t0)
    lw s6, 32(t0)
    lw s7, 36(t0)
    lw s8, 40(t0)
    lw s9, 44(t0)
    lw s10, 48(t0)
    lw s11, 52(t0)
    lw gp, 56(t0)
    lw tp, 60(t0)
    ret

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

# run_user(entry, user_sp, a0, a1): mret into U at entry with sp/a0/a1 and MPIE = 1 (the tick preempts U). Returns
# when m_trap ends the run (an exit ecall or a fault). mcause is not written (#612): mret restores mil from its mpil,
# which is 0 here because every earlier trap was taken at level 0.
.section .text, "ax"
.global run_user
run_user:
    la t0, k_ctx
    sw ra, 0(t0)
    sw sp, 4(t0)
    sw s0, 8(t0)
    sw s1, 12(t0)
    sw s2, 16(t0)
    sw s3, 20(t0)
    sw s4, 24(t0)
    sw s5, 28(t0)
    sw s6, 32(t0)
    sw s7, 36(t0)
    sw s8, 40(t0)
    sw s9, 44(t0)
    sw s10, 48(t0)
    sw s11, 52(t0)
    sw gp, 56(t0)
    sw tp, 60(t0)
    li t1, 0x1800               # MPP = U
    csrc mstatus, t1
    li t1, 0x80                 # MPIE = 1
    csrs mstatus, t1
    csrw mepc, a0
    mv sp, a1
    mv a0, a2
    mv a1, a3
    # U sees only sp, a0 and a1: every other register is zeroed (the callee-saved ones are in k_ctx already).
    mv ra, zero
    mv gp, zero
    mv tp, zero
    mv t0, zero
    mv t1, zero
    mv t2, zero
    mv t3, zero
    mv t4, zero
    mv t5, zero
    mv t6, zero
    mv a2, zero
    mv a3, zero
    mv a4, zero
    mv a5, zero
    mv a6, zero
    mv a7, zero
    mv s0, zero
    mv s1, zero
    mv s2, zero
    mv s3, zero
    mv s4, zero
    mv s5, zero
    mv s6, zero
    mv s7, zero
    mv s8, zero
    mv s9, zero
    mv s10, zero
    mv s11, zero
    mret

.section .bss
.balign 4
k_ctx: .space 64

# U-mode stubs, on their own 4 KB page (.utext, granted R+X to U). a7 = 0: echo (the kernel returns a0 + 1);
# a7 = 1: exit with a0.
.section .utext, "ax"
.balign 4
.global u_echo
u_echo:
    li a7, 0
    ecall
    li a7, 1
    ecall
.global u_load
u_load:
    lw a0, 0(a0)
    li a7, 1
    ecall
.global u_store
u_store:
    sw a1, 0(a0)
    li a7, 1
    ecall
# Spin a0 times (long enough for several ticks), then an illegal word: the fault must still arrive after preemption.
.global u_spin_illegal
u_spin_illegal:
    addi a0, a0, -1
    bnez a0, u_spin_illegal
.global u_illegal_at
u_illegal_at:
    .word 0
