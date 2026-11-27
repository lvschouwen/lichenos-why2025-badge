// PMP and PMA entry text for "fw pmp" (#608): what each entry covers and allows. Pure, host-tested in
// host_tests/pmp_fmt_test.c. Encodings: RISC-V privileged spec (PMP) and riscv/csr.h (the P4's PMA CSRs 0xBC0/0xBD0).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define PMP_FMT_ENTRIES 16 // rev v1.0: pmpcfg4+ and pmpaddr16+ are illegal CSRs (#608 run 10); PMA has 16 too

// One PMP entry: cfg is its pmpcfg byte, addr its pmpaddr, prev_addr pmpaddr[idx-1] (0 for entry 0; TOR's base).
// Writes "pmp<idx> <L|-> <OFF|TOR|NA4|NAPOT> <rwx> 0x<lo>-0x<hi> (<size>)", or "pmp<idx> off" when unused.
// Returns the length written (snprintf rules: never more than cap-1, always terminated when cap > 0).
size_t pmp_entry_format(char *out, size_t cap, unsigned idx, uint8_t cfg, uint32_t addr, uint32_t prev_addr);

// One PMA entry, same shape, plus the cache attributes: "nc" non-cacheable, "wt" write-through, "wmna"/"rmna"
// write/read miss no-allocate. An entry with EN clear prints "pma<idx> off" with its attribute bits if any are set.
size_t pma_entry_format(char *out, size_t cap, unsigned idx, uint32_t cfg, uint32_t addr, uint32_t prev_addr);
