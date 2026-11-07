#pragma once
// Notes app (#154): the pure rules. A note is one line of text in its own file; a to do starts with "[ ] " or "[x] ".
// ESP-free, host-tested in host_tests/notes_model_test.c; notes_store.c does the file work, app_notes.c the screens.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOTES_TEXT_MAX 120 // what a person types (the text_entry limit)
#define NOTES_LINE_MAX 128 // the file's line: prefix + text + NUL
#define NOTES_NAME_MAX 40  // "<24 letters>-NN.txt"
#define NOTES_PREFIX_LEN 4

typedef enum { NOTE_PLAIN, NOTE_TODO, NOTE_DONE } note_kind_t;

// Kind of a line; *body = the text after the prefix (the line itself for a plain note). "[X] " counts as done.
note_kind_t notes_parse(char const *line, char const **body);
// prefix + body into out; false (out empty) when it does not fit in cap or body is longer than NOTES_TEXT_MAX
bool notes_format(note_kind_t kind, char const *body, char *out, size_t cap);
// What ticking does: a plain note becomes a to do, a to do is done, a done one is a to do again
note_kind_t notes_next_kind(note_kind_t kind);

// The first line of raw[0..len): control characters (tabs included) become spaces, runs of blanks collapse, ends are
// trimmed, cut to cap-1 bytes on a UTF-8 boundary. Empty result for a blank line.
void notes_clean_line(char const *raw, size_t len, char *out, size_t cap);

// A file name from the note's text: lowercase ASCII letters and digits of the first words, joined by '-', at most 24
// characters, "note" when there are none; n = 0 gives "<slug>.txt", n > 0 "<slug>-<n>.txt" (the caller counts up
// until the name is free). False when it does not fit in cap.
bool notes_file_name(char const *body, unsigned n, char *out, size_t cap);
// A name the app lists: "<something>.txt", no '/', not hidden, at most NOTES_NAME_MAX - 1 bytes
bool notes_name_ok(char const *name);
// Listing order: by name without regard to case, then exactly
int notes_name_compare(char const *a, char const *b);

#ifdef __cplusplus
}
#endif
