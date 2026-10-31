#pragma once

#include <stddef.h>
#include <stdint.h>

// The badge's log (#38): every esp_log line and every console reply is kept in a PSRAM ring (log_ring.h) with a
// sequence number and an uptime, so GET /log can show what happened without the UART. Lines printed by ROM code
// (esp_rom_printf, panics, the bootloader) and by plain printf elsewhere never reach it.
#define LOG_STORE_BYTES (96 * 1024)

// Call in app_main after log_tap_start: hooks esp_log (the previous hook keeps running, so the UART output is unchanged).
void log_store_start(void);
// A line of console output (see console_set_tap). Safe from any task, never from an interrupt.
void log_store_add(char const *text, size_t len);
// Up to cap bytes of lines with sequence number >= since, as log_ring_read renders them. cap should be a few KB: the
// ring is locked (interrupts off) only while the raw records are copied; formatting happens after. Returns 0 bytes when there is nothing new or the store is off.
size_t log_store_read(uint32_t since, char *out, size_t cap, uint32_t *next, uint32_t *missed);
// Lines that were not stored because another writer held the ring at that moment
uint32_t log_store_lost(void);
