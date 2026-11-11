#include "display_timing.h"
#include "st7703_bono.h"

#include <stdio.h>

static int fails;

#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                                                             \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main(void) {
    // the panel's own totals (one source: st7703_bono.h)
    EXPECT(ST7703_HTOTAL == 1006);
    EXPECT(ST7703_VTOTAL == 764);
    EXPECT(disp_refresh_dhz(ST7703_DPI_CLOCK_MHZ * 1000000u, ST7703_HTOTAL, ST7703_VTOTAL) == 468); // 46.8 Hz
    EXPECT(disp_refresh_dhz(38000000, 1006, 755) == 500);                                              // #322: 50.0 Hz
    EXPECT(disp_refresh_dhz(47000000, 1006, 764) == 612);
    EXPECT(disp_refresh_dhz(36000000, 0, 764) == 0);
    EXPECT(disp_refresh_dhz(36000000, 1006, 0) == 0);
    // PLL_F240M with the divider field 6 (div 7) is not 36 MHz: the clock tree rounds, the registry shows what it did
    EXPECT(disp_pclk_hz(240000000, 5) == 40000000);
    EXPECT(disp_pclk_hz(240000000, 6) == 34285714);
    EXPECT(disp_pclk_hz(240000000, 0) == 240000000);
    if (fails == 0) {
        printf("display_timing_test: ok\n");
    }
    return fails ? 1 : 0;
}
