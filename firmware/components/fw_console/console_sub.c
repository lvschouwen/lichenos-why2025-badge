// A command's sub-command table (#464): console_sub.h
#include "console_sub.h"

#include <stdio.h>
#include <string.h>

char const *console_sub_match(console_sub_t const *sub, char const *args) {
    if (sub->match == CON_MATCH_REST) {
        return args;
    }
    if (!sub->word || sub->match == CON_MATCH_END) {
        return NULL; // a row compiled out of this image
    }
    size_t len = strlen(sub->word);
    if (sub->match == CON_MATCH_EXACT) {
        return strcmp(args, sub->word) == 0 ? args + len : NULL;
    }
    if (strncmp(args, sub->word, len) != 0) {
        return NULL;
    }
    if (sub->match == CON_MATCH_PREFIX) {
        return args + len;
    }
    if (args[len] != ' ' && args[len]) {
        return NULL; // another word that starts the same ("history" is not "hist")
    }
    return args + len + (sub->match == CON_MATCH_WORD && args[len] ? 1 : 0);
}

console_sub_t const *console_sub_find(console_sub_t const *table, char const *args) {
    for (console_sub_t const *row = table; row && row->match != CON_MATCH_END; row++) {
        if (console_sub_match(row, args)) {
            return row;
        }
    }
    return NULL;
}

bool console_origin_allows(uint8_t rule, console_origin_t origin) {
    switch (rule) {
        case CON_ORIGIN_ANY: return true;
        case CON_ORIGIN_LOCAL: return origin == CONSOLE_ORIGIN_UART || origin == CONSOLE_ORIGIN_LOCAL;
        case CON_ORIGIN_UART: return origin == CONSOLE_ORIGIN_UART;
        default: return false; // fail closed
    }
}

console_rule_t console_sub_rule(console_sub_t const *row, console_ask_t const *ask) {
    if (!row) {
        return CON_RULE_RUN;
    }
    if (row->gate != CON_GATE_OPEN && !ask->dev_on) {
        return CON_RULE_DEV;
    }
    if (!console_origin_allows(row->origin, ask->origin)) {
        return CON_RULE_ORIGIN;
    }
    if ((row->flags & CON_F_USB_HOLD) && ask->held && ask->origin != CONSOLE_ORIGIN_UART) { // #289's rule
        return CON_RULE_HOLD;
    }
    return CON_RULE_RUN;
}

static char const *origin_words(console_sub_t const *row) {
    if (row->origin == CON_ORIGIN_LOCAL) {
        return "only on the badge (the USB console or the Console app)";
    }
    if (row->flags & CON_F_ERASE) {
        return "it erases storage, so only the USB console can do it (the screen's Settings can too)";
    }
#ifdef CONFIG_LICHEN_DEV_COMMANDS // a release has no developer mode, so no hint to give
    if (row->flags & CON_F_DEV_HINT) {
        return "only from the USB console (or " DEV_MODE_ENTER_HINT ")";
    }
#endif
    return "only from the USB console";
}

char *console_sub_refusal(char *out, size_t cap, console_rule_t rule, char const *name, console_sub_t const *row,
                          char const *why) {
    size_t wl = row->match != CON_MATCH_REST && row->word ? strlen(row->word) : 0;
    wl -= wl && row->word[wl - 1] == ' '; // a PREFIX word's own space
    char const *word = wl ? row->word : "";
    switch (rule) {
        case CON_RULE_DEV:
            snprintf(out, cap, "%s%s%.*s: " CONSOLE_DEV_OFF_REPLY, name, wl ? " " : "", (int)wl, word);
            break;
        case CON_RULE_HOLD:
            snprintf(out, cap, "%s%s%.*s refused: %s is on; " HOLD_OFF_FIRST, name, wl ? " " : "", (int)wl, word,
                     why ? why : "?");
            break;
        case CON_RULE_ORIGIN:
        case CON_RULE_RUN:
        default:
            snprintf(out, cap, "%s%s%.*s refused: %s", name, wl ? " " : "", (int)wl, word, origin_words(row));
            break;
    }
    return out;
}
