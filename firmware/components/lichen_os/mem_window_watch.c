// "fw mem window [reset]" (#410): the heaps' figures fed to mem_window.c. IDF's own local-minimum monitor
// (heap_caps_monitor_local_minimum_free_size_start) is not used: while it runs, heap_caps_get_minimum_free_size and
// fw mem's min answer for the window instead of since boot, and its start allocates. Instead the window is sampled on
// dma_watch.c's existing 250 ms esp_timer tick and the heap's since-boot mark is folded in: a dip to a new since-boot
// low is exact, a shorter dip above it between two ticks can be missed. Nothing is added to malloc or free.
#include "mem_window_watch.h"

#ifdef CONFIG_LICHEN_DEV_COMMANDS
#include "mem_window.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

static uint32_t const CAPS[MEM_WINDOW_HEAPS] = {MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_DMA,
                                                MALLOC_CAP_SPIRAM}; // fw mem's three lines

static mem_window_t win; // ~50 B internal .bss under win_mux (a critical section never touches PSRAM)
static portMUX_TYPE win_mux = portMUX_INITIALIZER_UNLOCKED;

// Reads fields the heap keeps (no walk, no heap lock), as fw mem's figures with heap task tracking off
static void figures(mem_window_fig_t f[MEM_WINDOW_HEAPS]) {
    for (int i = 0; i < MEM_WINDOW_HEAPS; i++) {
        f[i].free_now = (uint32_t)heap_caps_get_free_size(CAPS[i]);
        f[i].boot_min = (uint32_t)heap_caps_get_minimum_free_size(CAPS[i]);
    }
}

static uint32_t now_ms(void) {
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000); // wraps after 49.7 days: mem_window.h
}

void mem_window_watch_sample(void) {
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figures(f);
    uint32_t at = now_ms();
    portENTER_CRITICAL(&win_mux);
    mem_window_sample(&win, f, at);
    portEXIT_CRITICAL(&win_mux);
}

static void cmd_window(char const *args) {
    if (!console_dev_gate("mem window")) {
        return;
    }
    bool reset = strcmp(args, "reset") == 0;
    if (!reset && args[0]) {
        console_reply("mem window: usage fw mem window [reset]");
        return;
    }
    mem_window_fig_t f[MEM_WINDOW_HEAPS];
    figures(f);
    uint32_t at = now_ms();
    portENTER_CRITICAL(&win_mux);
    if (reset) {
        mem_window_reset(&win, f, at);
    } else {
        mem_window_sample(&win, f, at); // the figures now count too
    }
    mem_window_t snap = win; // printed after the critical section: no lock across a reply
    portEXIT_CRITICAL(&win_mux);
    char line[96];
    for (unsigned i = 0; i < MEM_WINDOW_HEAPS; i++) {
        if (mem_window_line(&snap, i, at, line, sizeof(line))) {
            console_reply("%s", line);
        }
    }
}

void mem_window_watch_register(void) {
    console_set_mem_window(cmd_window);
}
#endif
