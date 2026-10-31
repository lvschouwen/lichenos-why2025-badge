// Type-to-search matching (#32, #57): see list_filter.h
#include "list_filter.h"

#include <ctype.h>
#include <string.h>

#define KEY_BACKSPACE 8
#define KEY_DEL       127

bool list_filter_edit(char *q, size_t cap, uint32_t key) {
    size_t len = strlen(q);
    if (key == KEY_BACKSPACE || key == KEY_DEL) {
        if (!len) {
            return false;
        }
        q[len - 1] = '\0';
        return true;
    }
    size_t max = cap - 1 < LIST_FILTER_MAX ? cap - 1 : LIST_FILTER_MAX;
    if (key < 0x20 || key > 0x7E || len >= max) {
        return false;
    }
    if (key == ' ' && (len == 0 || q[len - 1] == ' ')) {
        return false;
    }
    q[len]     = (char)key;
    q[len + 1] = '\0';
    return true;
}

bool list_filter_match(char const *q, list_filter_word_fn word_ok, void *ctx) {
    char word[LIST_FILTER_MAX + 1];
    while (*q) {
        while (*q == ' ') {
            q++;
        }
        size_t n = 0;
        while (q[n] && q[n] != ' ') {
            n++;
        }
        if (!n) {
            break;
        }
        if (n >= sizeof(word)) {
            n = sizeof(word) - 1;
        }
        for (size_t i = 0; i < n; i++) {
            word[i] = (char)tolower((unsigned char)q[i]);
        }
        word[n] = '\0';
        if (!word_ok(word, ctx)) {
            return false;
        }
        q += n;
    }
    return true;
}

bool list_filter_text_has(char const *text, char const *word) {
    if (!text) {
        return false;
    }
    size_t n = strlen(word);
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)word[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return n == 0;
}

bool list_filter_text_starts(char const *text, char const *word) {
    if (!text) {
        return false;
    }
    for (; *word; word++, text++) {
        if (tolower((unsigned char)*text) != tolower((unsigned char)*word)) {
            return false;
        }
    }
    return true;
}

bool list_filter_is_hex(char const *word) {
    if (!*word) {
        return false;
    }
    for (; *word; word++) {
        if (!isxdigit((unsigned char)*word)) {
            return false;
        }
    }
    return true;
}

bool list_filter_is_number(char const *word) {
    bool digit = false;
    for (; *word; word++) {
        if (isdigit((unsigned char)*word)) {
            digit = true;
        } else if (*word != '.') {
            return false;
        }
    }
    return digit;
}

bool list_filter_key_starts(uint8_t const *key, size_t n, char const *word) {
    static char const hex[] = "0123456789abcdef";
    if (!list_filter_is_hex(word)) {
        return false;
    }
    for (size_t i = 0; word[i]; i++) {
        if (i >= n * 2) {
            return false;
        }
        uint8_t b = key[i / 2];
        char    c = hex[i % 2 ? b & 15 : b >> 4];
        if (c != tolower((unsigned char)word[i])) {
            return false;
        }
    }
    return true;
}
