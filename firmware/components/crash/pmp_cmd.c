// "fw pmp" (#608, developer mode only): both cores' PMP and PMA as the running image has them, read on each core
// (PMP/PMA CSRs are per hart), plus the AXI ICM's CPU address-hole record: the first peripheral or L2MEM access the APM
// denied, which #608 run 9 found is where rev v1.0 keeps its "PMS violation record". Read-only.
#include "pmp_fmt.h"

#include "esp_ipc.h"
#include "fw_console.h"
#include "riscv/csr.h"
#include "soc/icm_sys_reg.h"
#include "soc/soc.h"

#include <stdint.h>

typedef struct {
    uint32_t pmpcfg[PMP_FMT_ENTRIES / 4];
    uint32_t pmpaddr[PMP_FMT_ENTRIES];
    uint32_t pmacfg[PMP_FMT_ENTRIES];
    uint32_t pmaaddr[PMP_FMT_ENTRIES];
    uint32_t mstatus;
} pmp_snap_t;

// CSR numbers are immediates, hence one read per entry.
#define RD_PMP(i)                                                                                                      \
    s->pmpaddr[i] = RV_READ_CSR(CSR_PMPADDR0 + (i));                                                                   \
    s->pmacfg[i]  = RV_READ_CSR(CSR_PMACFG0 + (i));                                                                    \
    s->pmaaddr[i] = RV_READ_CSR(CSR_PMAADDR0 + (i))

static void read_here(void *arg) {
    pmp_snap_t *s = arg;
    s->pmpcfg[0]  = RV_READ_CSR(CSR_PMPCFG0);
    s->pmpcfg[1]  = RV_READ_CSR(CSR_PMPCFG0 + 1);
    s->pmpcfg[2]  = RV_READ_CSR(CSR_PMPCFG0 + 2);
    s->pmpcfg[3]  = RV_READ_CSR(CSR_PMPCFG0 + 3);
    s->mstatus    = RV_READ_CSR(mstatus);
    RD_PMP(0);
    RD_PMP(1);
    RD_PMP(2);
    RD_PMP(3);
    RD_PMP(4);
    RD_PMP(5);
    RD_PMP(6);
    RD_PMP(7);
    RD_PMP(8);
    RD_PMP(9);
    RD_PMP(10);
    RD_PMP(11);
    RD_PMP(12);
    RD_PMP(13);
    RD_PMP(14);
    RD_PMP(15);
}

static void reply_core(int core, pmp_snap_t const *s) {
    char line[96];
    int  locked = 0;
    for (unsigned i = 0; i < PMP_FMT_ENTRIES; i++) {
        uint8_t const cfg = (uint8_t)(s->pmpcfg[i / 4] >> (8 * (i % 4)));
        locked += (cfg & 0x80) != 0;
        pmp_entry_format(line, sizeof line, i, cfg, s->pmpaddr[i], i ? s->pmpaddr[i - 1] : 0);
        console_reply("pmp: core %d %s", core, line);
    }
    for (unsigned i = 0; i < PMP_FMT_ENTRIES; i++) {
        pma_entry_format(line, sizeof line, i, s->pmacfg[i], s->pmaaddr[i], i ? s->pmaaddr[i - 1] : 0);
        console_reply("pmp: core %d %s", core, line);
    }
    console_reply("pmp: core %d: %d of %d PMP entries locked, mstatus 0x%08lx", core, locked, PMP_FMT_ENTRIES,
                  (unsigned long)s->mstatus);
}

static void cmd_pmp(char const *args) {
    if (args[0]) {
        console_reply("pmp (both cores' PMP and PMA, and the ICM access-violation record)");
        return;
    }
    static pmp_snap_t snap; // one console task runs commands; keeps 200 B off its stack
    for (int core = 0; core < 2; core++) {
        esp_err_t const err = esp_ipc_call_blocking((uint32_t)core, read_here, &snap);
        if (err != ESP_OK) {
            console_reply("pmp: core %d: IPC failed (%s)", core, esp_err_to_name(err));
            continue;
        }
        reply_core(core, &snap);
    }
    uint32_t const raw  = REG_READ(ICM_INT_RAW_REG);
    uint32_t const addr = REG_READ(ICM_CPU_ADDRHOLE_ADDR_REG);
    uint32_t const info = REG_READ(ICM_CPU_ADDRHOLE_INFO_REG);
    if (raw & ICM_REG_ICM_CPU_ADDRHOLE_INT_RAW) {
        console_reply("pmp: ICM record: denied %s at 0x%08lx by master %lu (0 core 0, 1 core 1), info 0x%08lx",
                      info & ICM_REG_ICM_CPU_ADDRHOLE_WR ? "write" : "read", (unsigned long)addr,
                      (unsigned long)(info & ICM_REG_ICM_CPU_ADDRHOLE_ID), (unsigned long)info);
    } else {
        console_reply("pmp: ICM record: none since boot or the last clear (int_raw 0x%08lx)", (unsigned long)raw);
    }
}

void crash_pmp_register(void) {
    console_register_dev("pmp", "pmp (both cores' PMP and PMA, and the ICM access-violation record; #608)", cmd_pmp);
}
