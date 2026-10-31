#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Type-to-search for the list screens (#32, #57). Pure logic, no ESP or LVGL headers (host-tested in
// host_tests/list_filter_test.c); menu_filter.c is the screen side. The query is words separated by spaces and a row
// shows when every word matches it.

#define LIST_FILTER_MAX 24 // characters in a query

// Applies one key to the query q (capacity cap): a printable ASCII character is appended (no leading or double space),
// Backspace (8) and Del (127) remove the last one. Returns whether q changed.
bool list_filter_edit(char *q, size_t cap, uint32_t key);

// Called for each word of the query (lower case, never empty); true when the row matches it
typedef bool (*list_filter_word_fn)(char const *word, void *ctx);
// An empty (or blank) query matches everything
bool list_filter_match(char const *q, list_filter_word_fn word_ok, void *ctx);

// Building blocks for a word_ok callback
bool list_filter_text_has(char const *text, char const *word); // case-insensitive substring; text may be NULL
bool list_filter_is_hex(char const *word);                     // only 0-9 and a-f (any case), at least one
bool list_filter_is_number(char const *word);                  // only digits and dots, at least one digit
bool list_filter_text_starts(char const *text, char const *word); // case-insensitive prefix
// The first n bytes of key as lower-case hex start with word (a hex word)
bool list_filter_key_starts(uint8_t const *key, size_t n, char const *word);
