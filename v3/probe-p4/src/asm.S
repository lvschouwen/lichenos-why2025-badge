# #608 probe: entry, the one trap handler, the M -> U harness and the U-mode stubs.
# Every instruction here is 4 bytes (norvc): the M-mode fault path skips a faulting instruction with mepc += 4.
.option norvc

.section .text.boot, "ax"
.global _start
_start:
    csrci mstatus, 8            # MIE off: the probe never takes an interrupt in M
    la t0, trap_entry
    ori t0, t0, 3               # CLIC mode (mtvec.MODE is hardwired to 3 on the P4)
    csrw mtvec, t0
    csrr t0, mhartid
    bnez t0, park
    la sp, _mstack_top
    la t0, _trap_stack_top
    csrw mscratch, t0           # mscratch always holds the trap stack top outside the handler
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

# Exceptions and non-vectored interrupts land here (mtvec base, 64-byte aligned at least).
# trap_rust returns 0 to resume the trapped context with mret, 1 to end a U run and return to the harness.
.section .text.trap, "ax"
.balign 256
.global trap_entry
trap_entry:
    csrrw sp, mscratch, sp
    addi sp, sp, -128
    sw x1, 4(sp)
    sw x3, 12(sp)
    sw x4, 16(sp)
    sw x5, 20(sp)
    sw x6, 24(sp)
    sw x7, 28(sp)
    sw x8, 32(sp)
    sw x9, 36(sp)
    sw x10, 40(sp)
    sw x11, 44(sp)
    sw x12, 48(sp)
    sw x13, 52(sp)
    sw x14, 56(sp)
    sw x15, 60(sp)
    sw x16, 64(sp)
    sw x17, 68(sp)
    sw x18, 72(sp)
    sw x19, 76(sp)
    sw x20, 80(sp)
    sw x21, 84(sp)
    sw x22, 88(sp)
    sw x23, 92(sp)
    sw x24, 96(sp)
    sw x25, 100(sp)
    sw x26, 104(sp)
    sw x27, 108(sp)
    sw x28, 112(sp)
    sw x29, 116(sp)
    sw x30, 120(sp)
    sw x31, 124(sp)
    csrr t0, mscratch
    sw t0, 8(sp)
    mv a0, sp
    call trap_rust
    bnez a0, to_harness
    lw x1, 4(sp)
    lw x3, 12(sp)
    lw x4, 16(sp)
    lw x5, 20(sp)
    lw x6, 24(sp)
    lw x7, 28(sp)
    lw x8, 32(sp)
    lw x9, 36(sp)
    lw x10, 40(sp)
    lw x11, 44(sp)
    lw x12, 48(sp)
    lw x13, 52(sp)
    lw x14, 56(sp)
    lw x15, 60(sp)
    lw x16, 64(sp)
    lw x17, 68(sp)
    lw x18, 72(sp)
    lw x19, 76(sp)
    lw x20, 80(sp)
    lw x21, 84(sp)
    lw x22, 88(sp)
    lw x23, 92(sp)
    lw x24, 96(sp)
    lw x25, 100(sp)
    lw x26, 104(sp)
    lw x27, 108(sp)
    lw x28, 112(sp)
    lw x29, 116(sp)
    lw x30, 120(sp)
    lw x31, 124(sp)
    addi sp, sp, 128
    csrrw sp, mscratch, sp
    mret
to_harness:
    addi t0, sp, 128
    csrw mscratch, t0
    la t0, m_ctx
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
    li a0, 0
    ret

# run_user(entry, user_sp, a0, a1, mpie): mret into U at entry with the given sp/a0/a1. Returns when the U code traps
# with an exception (trap_rust records it). mcause is cleared first so mret restores interrupt level 0 (CLIC mpil).
.section .text
.global run_user
run_user:
    la t0, m_ctx
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
    csrw mcause, zero
    li t1, 0x1880               # MPP = U, MPIE = 0
    csrc mstatus, t1
    beqz a4, 1f
    li t1, 0x80
    csrs mstatus, t1
1:  csrw mepc, a0
    mv sp, a1
    mv a0, a2
    mv a1, a3
    mret

.section .bss
.balign 4
.global m_ctx
m_ctx: .space 64

# U-mode stubs: each does one thing and ends with ecall (a0/a1 carry the result back).
.section .utext, "ax"
.balign 4
.global u_ecall
u_ecall:
    ecall
.global u_csr810
u_csr810:
    csrr a0, 0x810
    ecall
.global u_csr803
u_csr803:
    csrr a0, 0x803
    ecall
.global u_csr804
u_csr804:
    csrr a0, 0x804
    ecall
.global u_csr805
u_csr805:
    csrr a0, 0x805
    ecall
.global u_load
u_load:
    lw a0, 0(a0)
    ecall
.global u_store
u_store:
    sw a1, 0(a0)
    ecall
.global u_storeb
u_storeb:
    sb a1, 0(a0)
    ecall
.global u_spin
u_spin:
    addi a0, a0, -1
    bnez a0, u_spin
    ecall
.global u_mret
u_mret:
    mret
.global u_csrw_pmp
u_csrw_pmp:
    csrw pmpcfg0, zero
    ecall
