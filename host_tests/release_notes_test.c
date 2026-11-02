// Host test: release notes naming, ordering, the "what is new" marker and the markdown subset (#13)
#include "release_notes.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_names(void) {
    uint32_t seq;
    char     date[RN_DATE_LEN];
    CHECK(rn_parse_name("0003-2026-11-10.md", &seq, date) && seq == 3 && strcmp(date, "2026-11-10") == 0);
    CHECK(rn_parse_name("12-2027-01-02.md", &seq, NULL) && seq == 12);
    CHECK(!rn_parse_name("README.md", &seq, date));
    CHECK(!rn_parse_name("0003-2026-11-10.txt", &seq, date));
    CHECK(!rn_parse_name("0003-2026-11-1.md", &seq, date));
    CHECK(!rn_parse_name("-2026-11-10.md", &seq, date));
    CHECK(!rn_parse_name("", &seq, date) && !rn_parse_name(NULL, &seq, date));
}

static void test_index(void) {
    rn_file_t files[] = {{"0001-2026-11-02.md", "# one"}, {"junk.md", "x"}, {"0003-2026-11-09.md", "# three"},
                         {"0002-2026-11-05.md", "# two"}, {"0003-2026-11-10.md", "# dup"}};
    rn_note_t out[8];
    size_t    n = rn_build_index(files, 5, out, 8);
    CHECK(n == 3);
    CHECK(out[0].seq == 3 && strcmp(out[0].text, "# three") == 0 && strcmp(out[0].date, "2026-11-09") == 0);
    CHECK(out[1].seq == 2 && out[2].seq == 1);
    // A full table keeps the newest ones
    n = rn_build_index(files, 5, out, 2);
    CHECK(n == 2 && out[0].seq == 3 && out[1].seq == 2);
    CHECK(rn_build_index(files, 0, out, 8) == 0);
}

static void test_title(void) {
    char t[32];
    rn_title("\n## sub\n# What's new  \nbody", t, sizeof(t));
    CHECK(strcmp(t, "What's new") == 0);
    rn_title("just text", t, sizeof(t));
    CHECK(strcmp(t, "Release notes") == 0);
    rn_title("# A very long heading that does not fit", t, 8);
    CHECK(strcmp(t, "A very ") == 0);
}

static void test_marker(void) {
    bool known;
    CHECK(rn_parse_seen("12", &known) == 12 && known);
    CHECK(rn_parse_seen("", &known) == 0 && !known);
    CHECK(rn_parse_seen("12x", &known) == 0 && !known);
    CHECK(rn_parse_seen(NULL, &known) == 0 && !known);

    rn_note_t n[3] = {{.seq = 3}, {.seq = 2}, {.seq = 1}};
    CHECK(rn_count_new(n, 3, true, 1) == 2);
    CHECK(rn_count_new(n, 3, true, 3) == 0);
    CHECK(rn_count_new(n, 3, true, 9) == 0);
    CHECK(rn_count_new(n, 3, false, 0) == 3);
}

static void test_boot(void) {
    CHECK(rn_boot_decision(true, true, 1, 3) == RN_BOOT_SHOW);     // an update brought notes
    CHECK(rn_boot_decision(true, false, 0, 3) == RN_BOOT_SHOW);    // an update from before this feature
    CHECK(rn_boot_decision(true, true, 3, 3) == RN_BOOT_NOTHING);  // the same notes again: once per version
    CHECK(rn_boot_decision(true, true, 5, 3) == RN_BOOT_NOTHING);  // downgraded
    CHECK(rn_boot_decision(true, true, 0, 0) == RN_BOOT_NOTHING);  // no notes in the build
    CHECK(rn_boot_decision(false, false, 0, 3) == RN_BOOT_MARK);   // freshly flashed
    CHECK(rn_boot_decision(false, true, 1, 3) == RN_BOOT_NOTHING); // cable flash: the tile badge says it
}

static rn_kind_t next(char const **c, char *text) {
    rn_block_t b;
    rn_next_block(c, &b);
    strcpy(text, b.text);
    return b.kind;
}

static void test_markdown(void) {
    char const *md = "# Title\n\n## Section\n- one\n- two\n  continues\n* three\n\nA paragraph\nover two lines.\n"
                     "Another\n\n#tag is text\n";
    char const *c  = md;
    char        t[RN_TEXT_MAX];
    CHECK(next(&c, t) == RN_H1 && strcmp(t, "Title") == 0);
    CHECK(next(&c, t) == RN_H2 && strcmp(t, "Section") == 0);
    CHECK(next(&c, t) == RN_BULLET && strcmp(t, "one") == 0);
    CHECK(next(&c, t) == RN_BULLET && strcmp(t, "two continues") == 0);
    CHECK(next(&c, t) == RN_BULLET && strcmp(t, "three") == 0);
    CHECK(next(&c, t) == RN_PARA && strcmp(t, "A paragraph over two lines. Another") == 0);
    CHECK(next(&c, t) == RN_PARA && strcmp(t, "#tag is text") == 0);
    CHECK(next(&c, t) == RN_END);
    CHECK(next(&c, t) == RN_END); // stays at the end

    c = "";
    CHECK(next(&c, t) == RN_END);
    c = "\r\n  \r\nwindows\r\nlines\r\n";
    CHECK(next(&c, t) == RN_PARA && strcmp(t, "windows lines") == 0);
}

static void test_long(void) {
    char big[600];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char const *c = big;
    rn_block_t  b;
    rn_next_block(&c, &b);
    CHECK(b.kind == RN_PARA && strlen(b.text) == RN_TEXT_MAX - 1);
    rn_next_block(&c, &b);
    CHECK(b.kind == RN_END);
}

int main(void) {
    test_names();
    test_index();
    test_title();
    test_marker();
    test_boot();
    test_markdown();
    test_long();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("release_notes_test: ok\n");
    return 0;
}
