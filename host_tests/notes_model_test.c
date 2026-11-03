// Host test: the Notes app's rules, to do prefixes, file names, line cleaning (#154)
#include "notes_model.h"

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

static void test_parse(void) {
    char const *body;
    CHECK(notes_parse("buy milk", &body) == NOTE_PLAIN && strcmp(body, "buy milk") == 0);
    CHECK(notes_parse("[ ] buy milk", &body) == NOTE_TODO && strcmp(body, "buy milk") == 0);
    CHECK(notes_parse("[x] buy milk", &body) == NOTE_DONE && strcmp(body, "buy milk") == 0);
    CHECK(notes_parse("[X] buy milk", &body) == NOTE_DONE);
    CHECK(notes_parse("[x]buy", &body) == NOTE_PLAIN && strcmp(body, "[x]buy") == 0); // no blank after the box
    CHECK(notes_parse("[y] a", &body) == NOTE_PLAIN);
    CHECK(notes_parse("[ ] ", &body) == NOTE_TODO && body[0] == '\0');
    CHECK(notes_parse("", &body) == NOTE_PLAIN);
}

static void test_toggle(void) {
    char line[NOTES_LINE_MAX];
    CHECK(notes_next_kind(NOTE_PLAIN) == NOTE_TODO);
    CHECK(notes_next_kind(NOTE_TODO) == NOTE_DONE);
    CHECK(notes_next_kind(NOTE_DONE) == NOTE_TODO);
    CHECK(notes_format(NOTE_TODO, "milk", line, sizeof(line)) && strcmp(line, "[ ] milk") == 0);
    CHECK(notes_format(NOTE_DONE, "milk", line, sizeof(line)) && strcmp(line, "[x] milk") == 0);
    CHECK(notes_format(NOTE_PLAIN, "milk", line, sizeof(line)) && strcmp(line, "milk") == 0);
    // A round trip keeps the text
    char const *body;
    notes_format(notes_next_kind(NOTE_TODO), "milk", line, sizeof(line));
    CHECK(notes_parse(line, &body) == NOTE_DONE && strcmp(body, "milk") == 0);
}

static void test_format_limits(void) {
    char text[NOTES_TEXT_MAX + 2], line[NOTES_LINE_MAX], tiny[6];
    memset(text, 'a', sizeof(text));
    text[NOTES_TEXT_MAX] = '\0';
    CHECK(notes_format(NOTE_DONE, text, line, sizeof(line)) && strlen(line) == NOTES_TEXT_MAX + NOTES_PREFIX_LEN);
    text[NOTES_TEXT_MAX]     = 'a';
    text[NOTES_TEXT_MAX + 1] = '\0';
    CHECK(!notes_format(NOTE_PLAIN, text, line, sizeof(line)) && line[0] == '\0');
    CHECK(!notes_format(NOTE_TODO, "xy", tiny, sizeof(tiny)) && tiny[0] == '\0');
    CHECK(notes_format(NOTE_TODO, "x", tiny, sizeof(tiny)));
}

static void test_clean(void) {
    char out[32];
    notes_clean_line("  hello \t  world \r\nsecond", 26, out, sizeof(out));
    CHECK(strcmp(out, "hello world") == 0);
    notes_clean_line("\n\nx", 3, out, sizeof(out));
    CHECK(out[0] == '\0'); // the first line is blank
    notes_clean_line("   ", 3, out, sizeof(out));
    CHECK(out[0] == '\0');
    notes_clean_line("abcdefghij", 10, out, 6);
    CHECK(strcmp(out, "abcde") == 0);
    // Never half a character: "é" is 2 bytes, only one fits
    notes_clean_line("abc\xC3\xA9", 5, out, 5);
    CHECK(strcmp(out, "abc") == 0);
    notes_clean_line("abc\xC3\xA9", 5, out, 6);
    CHECK(strcmp(out, "abc\xC3\xA9") == 0);
    // The line is cut by the length given, not by what follows it
    notes_clean_line("abcdef", 3, out, sizeof(out));
    CHECK(strcmp(out, "abc") == 0);
}

static void test_names(void) {
    char name[NOTES_NAME_MAX];
    CHECK(notes_file_name("Buy milk & eggs!", 0, name, sizeof(name)) && strcmp(name, "buy-milk-eggs.txt") == 0);
    CHECK(notes_file_name("Buy milk & eggs!", 2, name, sizeof(name)) && strcmp(name, "buy-milk-eggs-2.txt") == 0);
    CHECK(notes_file_name("!!!", 0, name, sizeof(name)) && strcmp(name, "note.txt") == 0);
    CHECK(notes_file_name("\xC3\xA9\xC3\xA9", 0, name, sizeof(name)) && strcmp(name, "note.txt") == 0);
    CHECK(notes_file_name("  ../../etc/passwd", 0, name, sizeof(name)) && strcmp(name, "etc-passwd.txt") == 0);
    CHECK(notes_file_name("a very long first sentence of a note", 0, name, sizeof(name)) && strlen(name) <= 28);
    CHECK(notes_name_ok(name));
    CHECK(notes_file_name("a very long first sentence of a note", 99, name, sizeof(name)) && notes_name_ok(name));
    CHECK(!notes_file_name("milk", 0, name, 4) && name[0] == '\0');
}

static void test_name_ok(void) {
    CHECK(notes_name_ok("milk.txt") && notes_name_ok("a-1.txt"));
    CHECK(!notes_name_ok("milk.md") && !notes_name_ok(".txt") && !notes_name_ok(".hidden.txt"));
    CHECK(!notes_name_ok("a/b.txt") && !notes_name_ok("milk.new") && !notes_name_ok("") && !notes_name_ok(NULL));
    CHECK(!notes_name_ok("0123456789012345678901234567890123456.txt")); // 41 bytes
}

static void test_compare(void) {
    CHECK(notes_name_compare("a.txt", "B.txt") < 0);
    CHECK(notes_name_compare("b.txt", "A.txt") > 0);
    CHECK(notes_name_compare("a.txt", "a.txt") == 0);
    CHECK(notes_name_compare("a.txt", "a-1.txt") > 0); // '.' sorts after '-'
    CHECK(notes_name_compare("A.txt", "a.txt") != 0);
}

int main(void) {
    test_parse();
    test_toggle();
    test_format_limits();
    test_clean();
    test_names();
    test_name_ok();
    test_compare();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notes_model_test: ok\n");
    return 0;
}
