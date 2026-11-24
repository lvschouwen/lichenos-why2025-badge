/* SDIO RX staging release rule (#555): idle releases, busy and staged keep. */
#include <assert.h>
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

	puts("sdio_rx_idle_test ok");
	return 0;
}
