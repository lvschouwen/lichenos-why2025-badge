#pragma once
// A file as the ELF loader's byte source (#517, #523): plain read() into the loader's own PSRAM buffers, no stdio buffer
// (newlib would take it from internal RAM). Shared by the "fw elf run" spike and the SD apps' loader.

#include "esp_elf.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int    fd;
    size_t size;
} sd_elf_file_t;

// Opens path read-only: false with errno set (an empty file or a non-file is refused). On true the loader owns the
// descriptor: esp_elf_relocate_source closes it on every path.
bool sd_elf_open_source(char const *path, sd_elf_file_t *f);
// The loader's source over f; f must outlive the load.
elf_source_t sd_elf_source(sd_elf_file_t *f);
