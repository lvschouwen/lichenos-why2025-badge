// Host stand-in for export_shims.c (#519): only the size query it uses; export_shims_test.c defines it.
#pragma once
#include <stddef.h>
size_t heap_caps_get_allocated_size(void *ptr);
