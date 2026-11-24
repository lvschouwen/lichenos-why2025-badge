#pragma once

// "fw mem diff mark" / "fw mem diff [min bytes]" (#547): developer images only (CONFIG_LICHEN_DEV_COMMANDS); console.c
// calls it from "fw mem" and answers "not in this build" elsewhere
void mem_diff_command(char const *args);
