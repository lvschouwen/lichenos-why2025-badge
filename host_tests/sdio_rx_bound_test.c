/* SDIO RX read bound (#371): over the cap fails the link, the memory wait is bounded. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "eh_sdio_rx_bound.h"

int main(void)
{
	/* the cap: the INIT target when there is one, else 14 x 1536 B */
	assert(eh_sdio_rx_cap(21504, 1536, 0) == 21504);
	assert(eh_sdio_rx_cap(1536, 1536, 0) == 1536);
	assert(eh_sdio_rx_cap(0, 1536, 0) == 14u * 1536u);
	assert(eh_sdio_rx_cap(0, 1536, 512) == 21504); /* already whole blocks */
	assert(eh_sdio_rx_cap(0, 1000, 512) == 14336); /* 14000 rounded up */
	/* an INIT advertising more than the maximum cannot raise the cap */
	assert(eh_sdio_rx_cap(21505, 1536, 0) == 21504);
	assert(eh_sdio_rx_cap(255u * 1536u, 1536, 0) == 21504);
	assert(eh_sdio_rx_cap(255u * 512u, 1536, 512) == 21504);
	assert(eh_sdio_rx_step(255u * 1536u, eh_sdio_rx_cap(255u * 1536u, 1536, 0), 0, true, 0) ==
	       EH_SDIO_RX_FAIL);

	/* at the cap reads, one byte over fails whatever the memory */
	assert(eh_sdio_rx_step(21504, 21504, 0, true, 0) == EH_SDIO_RX_READ);
	assert(eh_sdio_rx_step(21505, 21504, 0, true, 0) == EH_SDIO_RX_FAIL);
	assert(eh_sdio_rx_step(21505, 21504, 0, false, 0) == EH_SDIO_RX_FAIL);
	/* a corrupt PKT_LEN (masked to 20 bits) never allocates */
	assert(eh_sdio_rx_step(0xFFFFF, 21504, 0, false, 0) == EH_SDIO_RX_FAIL);
	/* zero length is never a read */
	assert(eh_sdio_rx_step(0, 21504, 0, true, 0) == EH_SDIO_RX_FAIL);

	/* block-only reads: the rounded length counts */
	assert(eh_sdio_rx_step(21000, 21504, 512, true, 0) == EH_SDIO_RX_READ);
	assert(eh_sdio_rx_step(21505, 21504 + 512, 512, true, 0) == EH_SDIO_RX_READ);
	assert(eh_sdio_rx_step(21505, 21504, 512, true, 0) == EH_SDIO_RX_FAIL);
	assert(eh_sdio_rx_round(1, 512) == 512 && eh_sdio_rx_round(512, 512) == 512);

	/* under the cap without memory: wait until the bound, then fail */
	assert(eh_sdio_rx_step(3072, 21504, 0, false, 0) == EH_SDIO_RX_WAIT);
	assert(eh_sdio_rx_step(3072, 21504, 0, false, EH_SDIO_RX_MEM_WAIT_MAX_MS - 1) == EH_SDIO_RX_WAIT);
	assert(eh_sdio_rx_step(3072, 21504, 0, false, EH_SDIO_RX_MEM_WAIT_MAX_MS) == EH_SDIO_RX_FAIL);
	/* memory found late still reads */
	assert(eh_sdio_rx_step(3072, 21504, 0, true, EH_SDIO_RX_MEM_WAIT_MAX_MS) == EH_SDIO_RX_READ);

	printf("sdio_rx_bound_test: ok\n");
	return 0;
}
