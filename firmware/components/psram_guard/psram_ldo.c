#include "psram_ldo.h"

#include "esp_log.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/ldo_ll.h"
#include "sdkconfig.h"
#include "soc/efuse_struct.h"
#include "soc/pmu_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The PSRAM supply (#86 diagnosis). LDO channel 2 = unit 1 = VO2 powers the PSRAM and the MPLL. The IDF sets it to
// CONFIG_ESP_LDO_VOLTAGE_PSRAM_DOMAIN (1800, the only choice in esp-idf 6.1); for exactly 1800 mV the hal takes the
// eFuse-trimmed register pair instead of the formula. BadgeVMS ran this badge with a patched Kconfig at 1900 mV, which
// is the formula's dref 7 / mul 5: a different operating point. This command moves the supply at runtime so the bad
// pages can be measured (qwatch, dense test) at other settings; a reboot restores the IDF's value.

#define UNIT   1 // LDO unit of channel 2 (LDO_ID2UNIT)
#define IDX    3 // its PMU.ext_ldo index (ldo_ll index_array[1])
#define MIN_MV 1700
#define MAX_MV 2100 // nominal; the PSRAM is a 1.8 V part, stay well under its absolute maximum

static char const TAG[] = "psram";

static unsigned nominal(uint8_t dref, uint8_t mul) {
    unsigned vref = dref < 9 ? 500u + 50u * dref : 1000u + 100u * (dref - 9u);
    return vref * (4u + mul) / 4u;
}

static void show(void) {
    uint8_t dref = (uint8_t)PMU.ext_ldo[IDX].pmu_ext_ldo_ana.dref, mul = (uint8_t)PMU.ext_ldo[IDX].pmu_ext_ldo_ana.mul;
    uint8_t edref = (uint8_t)EFUSE.rd_mac_sys_2.ldo_vo2_dref, emul = (uint8_t)EFUSE.rd_mac_sys_3.ldo_vo2_mul;
    console_reply("psram ldo: VO2 (PSRAM + MPLL) dref %u mul %u = nominal %u mV%s; eFuse 1.8 V trim dref %u mul %u (nominal %u); "
                  "configured %d mV; tieh %u xpd %u force_tieh_sel %u",
                  dref, mul, nominal(dref, mul), dref == edref && mul == emul ? " (the trim pair)" : "", edref, emul,
                  nominal(edref, emul), CONFIG_ESP_LDO_VOLTAGE_PSRAM_DOMAIN, (unsigned)PMU.ext_ldo[IDX].pmu_ext_ldo.tieh,
                  (unsigned)PMU.ext_ldo[IDX].pmu_ext_ldo.xpd, (unsigned)PMU.ext_ldo[IDX].pmu_ext_ldo.force_tieh_sel);
    console_reply("psram ldo <mV> (%u..%u) sets it until reboot by the IDF formula: 1900 -> dref 7 mul 5 (BadgeVMS), 1950 -> 12/2 (= the trim), "
                  "2000 -> 6/6; psram ldo raw <dref 0..15> <mul 0..7> sets the pair", MIN_MV, MAX_MV);
}

void psram_ldo_cmd(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (!*args) {
        show();
        return;
    }
    uint8_t dref, mul;
    if (strncmp(args, "raw", 3) == 0) {
        int d = -1, m = -1;
        sscanf(args + 3, "%d %d", &d, &m);
        if (d < 0 || d > 15 || m < 0 || m > 7) {
            console_reply("psram ldo raw <dref 0..15> <mul 0..7>");
            return;
        }
        dref = (uint8_t)d;
        mul  = (uint8_t)m;
    } else {
        int mv = atoi(args);
        if (mv < MIN_MV || mv > MAX_MV) {
            console_reply("psram ldo <mV>: %u..%u", MIN_MV, MAX_MV);
            return;
        }
        bool rail;
        ldo_ll_voltage_to_dref_mul(UNIT, mv, &dref, &mul, &rail);
    }
    unsigned nom = nominal(dref, mul);
    if (nom > MAX_MV) {
        console_reply("psram ldo: dref %u mul %u is nominal %u mV, above the %u mV cap", dref, mul, nom, MAX_MV);
        return;
    }
    uint8_t was_d = (uint8_t)PMU.ext_ldo[IDX].pmu_ext_ldo_ana.dref, was_m = (uint8_t)PMU.ext_ldo[IDX].pmu_ext_ldo_ana.mul;
    ldo_ll_adjust_voltage(UNIT, dref, mul, false);
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_LOGW(TAG, "ldo: PSRAM supply changed dref %u mul %u -> %u %u (nominal %u -> %u mV) until reboot", was_d, was_m, dref, mul,
             nominal(was_d, was_m), nom);
    show();
}
