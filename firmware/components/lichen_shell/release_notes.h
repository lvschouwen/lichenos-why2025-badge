#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Release notes (#13). Pure and host-tested in host_tests/release_notes_test.c; the LVGL side is notes_app.c and
// notes_render.c.
//
// The notes are markdown files in firmware/release_notes/, named <sequence>-<YYYY-MM-DD>.md ("0003-2026-11-10.md"),
// compiled into the image so they always match the build. Firmware versions are fw-<sha> with no order of their own, so
// the sequence number is the order: a note with a higher sequence is newer. The badge remembers the highest sequence
// the person has seen (NVS notes_seen); "new" is every note above it.

#define RN_MAX_NOTES 32
#define RN_TEXT_MAX  256 // one rendered block (a paragraph, a bullet), longer is cut
#define RN_DATE_LEN  11  // "2026-11-02" and the NUL

typedef struct {
    char const *name; // file name, for the sequence and date
    char const *text; // NUL-terminated markdown
} rn_file_t;

// The files compiled into the image (generated at build time, notes_table in the component's build directory)
extern rn_file_t const rn_files[];
extern size_t const    rn_file_count;

typedef struct {
    uint32_t    seq;
    char        date[RN_DATE_LEN];
    char const *text;
} rn_note_t;

// "0003-2026-11-10.md" -> 3 and "2026-11-10"; false when the name does not have that shape. date may be NULL.
bool rn_parse_name(char const *name, uint32_t *seq, char *date);
// The notes of files[] with a valid name, newest (highest sequence) first, at most max; returns how many. Two files with
// the same sequence: the first listed wins.
size_t rn_build_index(rn_file_t const *files, size_t n, rn_note_t *out, size_t max);
// The first "# " heading's text, or "Release notes" when there is none. Returns the length written.
size_t rn_title(char const *text, char *out, size_t cap);

// The marker (NVS notes_seen, a decimal string); known=false when missing or not a number
uint32_t rn_parse_seen(char const *value, bool *known);
// Notes with a sequence above the marker; all of them when the marker is not known
size_t rn_count_new(rn_note_t const *notes, size_t n, bool seen_known, uint32_t seen);

typedef enum {
    RN_BOOT_NOTHING, // nothing to show, nothing to remember
    RN_BOOT_SHOW,    // "This is new": show the unread notes, then remember them
    RN_BOOT_MARK,    // a fresh badge: everything counts as seen, silently
} rn_boot_t;
// What to do at boot. updated: this boot follows a successful update. A badge that never recorded a marker and was not
// updated is freshly flashed and has nothing "new" to say; one that is flashed over a cable keeps its marker, so the
// notes show as a badge on the Release Notes tile instead of a screen.
rn_boot_t rn_boot_decision(bool updated, bool seen_known, uint32_t seen, uint32_t newest);

// Markdown subset: "# " and "## " headings, "- " or "* " bullets (indented lines continue them), paragraphs (consecutive
// lines are joined with a space), blank lines separate. Anything else is a paragraph.
typedef enum { RN_END, RN_H1, RN_H2, RN_BULLET, RN_PARA } rn_kind_t;
typedef struct {
    rn_kind_t kind;
    char      text[RN_TEXT_MAX];
} rn_block_t;
// Reads the next block at *cursor and moves it on; kind RN_END at the end of the text
void rn_next_block(char const **cursor, rn_block_t *out);
