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

# Core 1 entry: core 0 hands this address to the ROM (ets_set_appcpu_boot_addr). Own stacks, own mtvec and mscratch;
# bss belongs to core 0 (already zeroed).
.global _start1
_start1:
    csrci mstatus, 8
    la t0, trap_entry
    ori t0, t0, 3
    csrw mtvec, t0
    la sp, _mstack1_top
    la t0, _trap_stack1_top
    csrw mscratch, t0
    call p1main
    j park

# B5.upper: one 8-byte stub per CSR (csrr a0; ret), pmpcfg4-15 then pmpaddr16-63. Called guarded: a trap on the
# csrr is skipped (mepc += 4) and the ret returns a0 as the caller set it.
.section .text
.balign 4
.global csr_upper_tab
csr_upper_tab:
    csrr a0, 0x3a4
    ret
    csrr a0, 0x3a5
    ret
    csrr a0, 0x3a6
    ret
    csrr a0, 0x3a7
    ret
    csrr a0, 0x3a8
    ret
    csrr a0, 0x3a9
    ret
    csrr a0, 0x3aa
    ret
    csrr a0, 0x3ab
    ret
    csrr a0, 0x3ac
    ret
    csrr a0, 0x3ad
    ret
    csrr a0, 0x3ae
    ret
    csrr a0, 0x3af
    ret
    csrr a0, 0x3c0
    ret
    csrr a0, 0x3c1
    ret
    csrr a0, 0x3c2
    ret
    csrr a0, 0x3c3
    ret
    csrr a0, 0x3c4
    ret
    csrr a0, 0x3c5
    ret
    csrr a0, 0x3c6
    ret
    csrr a0, 0x3c7
    ret
    csrr a0, 0x3c8
    ret
    csrr a0, 0x3c9
    ret
    csrr a0, 0x3ca
    ret
    csrr a0, 0x3cb
    ret
    csrr a0, 0x3cc
    ret
    csrr a0, 0x3cd
    ret
    csrr a0, 0x3ce
    ret
    csrr a0, 0x3cf
    ret
    csrr a0, 0x3d0
    ret
    csrr a0, 0x3d1
    ret
    csrr a0, 0x3d2
    ret
    csrr a0, 0x3d3
    ret
    csrr a0, 0x3d4
    ret
    csrr a0, 0x3d5
    ret
    csrr a0, 0x3d6
    ret
    csrr a0, 0x3d7
    ret
    csrr a0, 0x3d8
    ret
    csrr a0, 0x3d9
    ret
    csrr a0, 0x3da
    ret
    csrr a0, 0x3db
    ret
    csrr a0, 0x3dc
    ret
    csrr a0, 0x3dd
    ret
    csrr a0, 0x3de
    ret
    csrr a0, 0x3df
    ret
    csrr a0, 0x3e0
    ret
    csrr a0, 0x3e1
    ret
    csrr a0, 0x3e2
    ret
    csrr a0, 0x3e3
    ret
    csrr a0, 0x3e4
    ret
    csrr a0, 0x3e5
    ret
    csrr a0, 0x3e6
    ret
    csrr a0, 0x3e7
    ret
    csrr a0, 0x3e8
    ret
    csrr a0, 0x3e9
    ret
    csrr a0, 0x3ea
    ret
    csrr a0, 0x3eb
    ret
    csrr a0, 0x3ec
    ret
    csrr a0, 0x3ed
    ret
    csrr a0, 0x3ee
    ret
    csrr a0, 0x3ef
    ret

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
# #609 check 5: U writes a1 to a dedicated-GPIO CSR, then reads it back into a0.
.global u_csrw803
u_csrw803:
    csrw 0x803, a1
    csrr a0, 0x803
    ecall
.global u_csrw804
u_csrw804:
    csrw 0x804, a1
    csrr a0, 0x804
    ecall
.global u_csrw805
u_csrw805:
    csrw 0x805, a1
    csrr a0, 0x805
    ecall
