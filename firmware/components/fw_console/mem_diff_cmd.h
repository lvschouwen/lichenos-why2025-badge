#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// "fw mem diff mark" / "fw mem diff [min bytes]" (#547): developer images only (CONFIG_LICHEN_DEV_COMMANDS); console.c
// calls it from "fw mem" and answers "not in this build" elsewhere
void mem_diff_command(char const *args);

// Names the task that allocated a block (lichen_os's dma_watch.c); set once at boot, before the first diff. Not in
// fw_console.h: that one is the app-facing gate manifest
void console_set_mem_owner(bool (*owner)(uint32_t addr, uint32_t size, char *task, size_t cap));
