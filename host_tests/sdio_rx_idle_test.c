/* SDIO RX staging release rule (#555): idle releases, busy and staged keep. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "eh_sdio_rx_idle.h"

int main(void)
{
	/* quiet for the full threshold, nothing staged: release */
	assert(eh_sdio_rx_release_due(0, EH_SDIO_RX_IDLE_MS));
	assert(eh_sdio_rx_release_due(0, EH_SDIO_RX_IDLE_MS * 10));
	/* traffic seen recently: keep (a stream never reaches the threshold) */
	assert(!eh_sdio_rx_release_due(0, 0));
	assert(!eh_sdio_rx_release_due(0, EH_SDIO_RX_IDLE_MS - 1));
	/* a slot staged for the parse task: keep, however long the quiet */
	assert(!eh_sdio_rx_release_due(1, EH_SDIO_RX_IDLE_MS));
	assert(!eh_sdio_rx_release_due(2, EH_SDIO_RX_IDLE_MS * 10));

	/* floor: ordinary single-frame slots stay, aggregates go */
	assert(!eh_sdio_rx_slot_over_floor(0));
	assert(!eh_sdio_rx_slot_over_floor(1536 + 512));
	assert(!eh_sdio_rx_slot_over_floor(EH_SDIO_RX_KEEP_FLOOR));
	assert(eh_sdio_rx_slot_over_floor(EH_SDIO_RX_KEEP_FLOOR + 1));
	assert(eh_sdio_rx_slot_over_floor(14 * 1536));

	/* plan: staged slot frees nothing */
	{
		size_t sz[2] = { 21504, 21504 };
		uint32_t mask = 99;
		assert(eh_sdio_rx_release_plan(sz, 2, 1, EH_SDIO_RX_IDLE_MS, &mask) == 0);
		assert(mask == 0);
		/* not yet quiet: nothing */
		assert(eh_sdio_rx_release_plan(sz, 2, 0, 10, &mask) == 0 && mask == 0);
		/* idle, both over the floor */
		assert(eh_sdio_rx_release_plan(sz, 2, 0, EH_SDIO_RX_IDLE_MS, &mask) == 43008);
		assert(mask == 3);
	}
	/* idle, one over and one under the floor */
	{
		size_t sz[2] = { 1536, 7680 };
		uint32_t mask;
		assert(eh_sdio_rx_release_plan(sz, 2, 0, EH_SDIO_RX_IDLE_MS, &mask) == 7680);
		assert(mask == 2);
	}
	/* already empty / unallocated */
	{
		size_t sz[2] = { 0, 0 };
		uint32_t mask = 7;
		assert(eh_sdio_rx_release_plan(sz, 2, 0, EH_SDIO_RX_IDLE_MS * 4, &mask) == 0);
		assert(mask == 0);
	}

	puts("sdio_rx_idle_test ok");
	return 0;
}
