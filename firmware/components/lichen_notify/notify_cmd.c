// "fw notify" (#62): the history, raising a test notice from the console task (the bench's way to see a toast),
// clearing and do-not-disturb
#include "esp_timer.h"
#include "fw_console.h"
#include "notify_os.h"

#include <stdio.h>
#include <string.h>

static char const *const LEVEL_NAME[] = {"info", "warn", "error"};

static void list(void) {
    ln_stats_t st;
    lichen_notify_stats(&st);
    if (!st.started) {
        console_reply("notify: not started");
        return;
    }
    char save[40] = "never this boot";
    if (st.last_save_ms) {
        snprintf(save, sizeof(save), "%lld s ago", (long long)((esp_timer_get_time() / 1000 - st.last_save_ms) / 1000));
    }
    console_reply("notify: %u entries, %u unread, dropped %u, muted %u, dnd %s, boot %u, %s, saves %u (errors %u), last %s",
                  (unsigned)st.entries, (unsigned)st.unread, (unsigned)st.dropped, (unsigned)st.muted, st.dnd ? "on" : "off",
                  (unsigned)st.boot, st.storage ? "kept in " "/storage/lichen/notify.bin" : "RAM only (no drive)",
                  (unsigned)st.saves, (unsigned)st.save_errors, save);
    uint32_t ids[NR_SLOTS];
    size_t   n      = lichen_notify_ids(ids, NR_SLOTS);
    uint32_t now_s  = (uint32_t)(esp_timer_get_time() / 1000000);
    for (size_t i = 0; i < n; i++) {
        nr_entry_t e;
        if (!lichen_notify_get(ids[i], &e)) {
            continue;
        }
        char age[24];
        if (e.boot == st.boot) {
            snprintf(age, sizeof(age), "%us ago", (unsigned)(now_s - e.mono_s));
        } else {
            snprintf(age, sizeof(age), "boot %u", (unsigned)e.boot);
        }
        console_reply("  #%u %-5s %c x%u %s %s%s%s: %s | %s", (unsigned)e.id, LEVEL_NAME[e.level], e.read ? 'R' : 'U',
                      (unsigned)e.count, age, e.source, e.target[0] ? "/" : "", e.target, e.title, e.body);
    }
}

static void test(char const *args) {
    int level = -1;
    for (int i = 0; i < 3; i++) {
        size_t len = strlen(LEVEL_NAME[i]);
        if (strncmp(args, LEVEL_NAME[i], len) == 0 && (args[len] == '\0' || args[len] == ' ')) {
            level = i;
            args += len;
        }
    }
    if (level < 0) {
        console_reply("notify test <info|warn|error> [text]");
        return;
    }
    while (*args == ' ') {
        args++;
    }
    char title[NR_TITLE_CAP];
    snprintf(title, sizeof(title), "Test %s", LEVEL_NAME[level]);
    // Target = the level: repeats of one level merge (count goes up), levels stay apart
    uint32_t id = lichen_notify_to("test", (ln_level_t)level, LEVEL_NAME[level], title,
                                   *args ? args : "Raised with fw notify test", LN_COALESCE);
    console_reply("notify test %s: %s %u", LEVEL_NAME[level], id ? "id" : "dropped", (unsigned)id);
}

static void cmd_notify(char const *args) {
    if (!*args) {
        list();
    } else if (strncmp(args, "test", 4) == 0 && (args[4] == ' ' || args[4] == '\0')) {
        test(args[4] ? args + 5 : "");
    } else if (strcmp(args, "clear") == 0) {
        lichen_notify_clear_all();
        console_reply("notify: cleared");
    } else if (strcmp(args, "read") == 0) {
        lichen_notify_mark_all_read();
        console_reply("notify: all read");
    } else if (strcmp(args, "dnd on") == 0 || strcmp(args, "dnd off") == 0) {
        lichen_notify_set_dnd(args[5] == 'n');
        console_reply("notify: do-not-disturb %s", lichen_notify_dnd() ? "on" : "off");
    } else {
        console_reply("notify | notify test <info|warn|error> [text] | notify clear | notify read | notify dnd on|off");
    }
}

void notify_cmd_register(void) {
    console_register("notify", "notify [test <info|warn|error> [text] | clear | read | dnd on|off]", cmd_notify);
}
