#pragma once
// Notes app (#154): the file work, plain POSIX so it is host-tested on a temp directory (notes_store_test.c). One note
// is one "<name>.txt" file holding one line. A save goes to "<name>.new" first and replaces the note when complete, so a
// note is whole or unchanged. The caller holds the SD card's batch lock (sd_begin/sd_end) and is not the LVGL task.

#include "notes_model.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[NOTES_NAME_MAX];
    char line[NOTES_LINE_MAX]; // the file's first line, cleaned
} notes_item_t;

typedef enum {
    NOTES_OK,
    NOTES_ERR_ARG,   // a bad name or an empty line
    NOTES_ERR_OPEN,  // the folder or file cannot be opened
    NOTES_ERR_SPACE, // the card is full
    NOTES_ERR_IO,
    NOTES_ERR_GONE,  // the note is not there (any more)
    NOTES_ERR_BUSY,  // EACCES/EBUSY: another task has the file open (FatFs's lock #274, LittleFS #300), or read-only
} notes_result_t;

// Lists dir's notes sorted by name into out (at most max); *count = stored, *skipped = notes left out (no room, empty
// file, unreadable). NOTES_ERR_OPEN when the folder cannot be opened.
notes_result_t notes_store_list(char const *dir, notes_item_t *out, size_t max, size_t *count, size_t *skipped);
// A new note: its file name comes from the text (notes_file_name), counted up until it is free. name_out gets it.
notes_result_t notes_store_add(char const *dir, char const *line, char *name_out, size_t cap);
// Replaces an existing note's line
notes_result_t notes_store_write(char const *dir, char const *name, char const *line);
notes_result_t notes_store_delete(char const *dir, char const *name);

char const *notes_result_text(notes_result_t r); // a sentence for the screen

#ifdef __cplusplus
}
#endif
