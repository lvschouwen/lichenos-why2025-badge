#include "reg_p4.h"

// The P4's registry keys of domain 0x12: disp.* (#312, row file split #315). Own file so the wave-2 agent of this
// domain touches nothing else. Empty until its issue lands: C has no zero-length array, so rows is NULL with count 0.
// To fill it, define `static reg_entry_t const rows[] = {...};` here (ids REG_ID_* from shared/reg/reg_ids.h, all of
// domain 0x12) and set `.rows = rows, .count = sizeof(rows) / sizeof(rows[0])` below. Keep ESP calls out of this file
// (the getters/setters come from lichen_reg.c, stubbed by the host test) so the host test can link it.
reg_p4_dom_t const reg_p4_display = {.rows = NULL, .count = 0};
