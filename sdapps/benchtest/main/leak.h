#pragma once
// Bench-only (#536): see leak.c.

// Takes 2 x 4 KB through malloc and keeps them: the app never frees them
void bench_leak_shim_blocks(void);
