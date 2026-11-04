// A repeater's settings (#117): see mesh_rset_run.h
#include "mesh_rset_run.h"

#include "lichen_mem.h"
#include "lichen_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "meshcore_run.h"

#include <stdio.h>
#include <string.h>

#define LATE_PART_MS (RR_APPEND_MS + 500) // our tagged reply may follow a stray one by this much

static void wipe(void *p, size_t n) {
    volatile char *v = p; // volatile: a wipe right before the buffer goes out of scope is not optimised away
    while (n--) {
        *v++ = 0;
    }
}

static uint32_t now_ms(void) {
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    return t ? t : 1;
}

void rs_run_init(rs_run_t *x, uint8_t const prefix[6]) {
    memset(x, 0, sizeof(*x));
    rs_init(&x->rs);
    memcpy(x->target, prefix, 6);
}

bool rs_run_busy(rs_run_t const *x) {
    return x->seq || rs_busy(&x->rs);
}

// Our request ended (or was replaced by someone else's): its answer to rs. false while a reply that is not ours may
// still be followed by ours.
static bool settle(rs_run_t *x, bool have) {
    rr_head_t const *r = &x->head;
    if (!have || r->seq != x->seq) {
        x->why = have ? "another request to a repeater was started (the console?)" : "MeshCore is not running";
        rs_on_answer(&x->rs, NULL);
        return true;
    }
    rr_exchange_t const *e     = &r->cli;
    bool                 mine  = r->cli_n && memcmp(r->cli_prefix, x->target, 6) == 0;
    char const          *reply = r->state == RR_DONE && mine && e->state == RR_CLI_GOT ? e->reply : NULL;
    if (!reply) {
        x->why = r->why;
        rs_on_answer(&x->rs, NULL); // failed or timed out
        return true;
    }
    if (rs_on_answer(&x->rs, reply)) {
        x->why = NULL;
        return true;
    }
    if (!x->not_ours_ms) {
        x->not_ours_ms = now_ms();
    }
    if (now_ms() - x->not_ours_ms < LATE_PART_MS) {
        return false;
    }
    x->why = "no answer";
    rs_on_answer(&x->rs, NULL);
    return true;
}

static bool head_busy(rr_head_t const *h) {
    return h->state == RR_QUEUED || h->state == RR_TAKEN || h->state == RR_SENT;
}

bool rs_run_admin(rs_run_t *x) {
    return mesh_remote_head(&x->head) && rr_head_logged_in(&x->head, x->target) && x->head.admin;
}

bool rs_run_step(rs_run_t *x) {
    bool have    = mesh_remote_head(&x->head);
    bool changed = false;
    if (x->seq) {
        if (have && x->head.seq == x->seq && head_busy(&x->head)) {
            return false; // still on its way
        }
        if (!settle(x, have)) {
            return false;
        }
        x->seq         = 0;
        x->not_ours_ms = 0;
        changed        = true;
    }
    if (have && head_busy(&x->head)) {
        return changed; // someone else's request: ours waits its turn
    }
    char cmd[RS_CMD_MAX + 1];
    if (rs_next(&x->rs, cmd, sizeof(cmd))) {
        char const *why = NULL;
        x->seq          = mesh_remote_start(x->target, RR_CLI, cmd, &why);
        wipe(cmd, sizeof(cmd)); // may hold a new password
        if (!x->seq) {
            x->why = why;
            rs_on_unsent(&x->rs, why);
        }
        changed = true;
    }
    return changed;
}

// ---- the console's read-only "settings" ---------------------------------------------------------------------------

#define CON_STACK   4096
#define CON_STEP_MS 200
#define CON_CAP_MS  (5 * 60 * 1000)
#define LOGIN_CAP_MS (RR_CAP_MS + 5000)

static rs_run_t *con;         // PSRAM: the last read, kept for "show"
static bool      con_running; // only the console task starts a read and only the job ends it

static void print_row(size_t i) {
    char text[RS_VALUE_MAX + 8];
    rs_value_text(&con->rs, i, text, sizeof(text));
    console_reply("  %-18s %s", rs_row_at(i)->label, text);
}

// Logs in with the remembered password when this badge is not logged in as admin; false (said why) when that failed
static bool con_login(void) {
    if (rs_run_admin(con)) {
        return true;
    }
    char pw[MF_PASSWORD_MAX + 2];
    if (!meshcore_pw_get(con->target, pw, sizeof(pw))) {
        console_reply("settings: no password remembered: fw mesh remote <who> login <password> first");
        return false;
    }
    char const *why = NULL;
    uint32_t    seq = mesh_remote_start(con->target, RR_LOGIN, pw, &why);
    wipe(pw, sizeof(pw));
    if (!seq) {
        console_reply("settings: login not started: %s", why ? why : "?");
        return false;
    }
    console_reply("settings: logging in with the remembered password...");
    for (uint32_t waited = 0; waited < LOGIN_CAP_MS; waited += CON_STEP_MS) {
        vTaskDelay(pdMS_TO_TICKS(CON_STEP_MS));
        if (!mesh_remote_head(&con->head) || con->head.seq != seq) {
            console_reply("settings: the login was replaced by another request");
            return false;
        }
        if (!head_busy(&con->head)) {
            break;
        }
    }
    rr_head_t const *h = &con->head;
    if (h->state != RR_DONE) {
        console_reply("settings: %s", h->why ? h->why : "no answer to the login");
        return false;
    }
    if (!h->admin) {
        console_reply("settings: logged in, but not as admin: the CLI needs admin");
        return false;
    }
    console_reply("settings: logged in as admin");
    return true;
}

static void con_task(void *arg) {
    (void)arg;
    if (con_login()) {
        rs_read_all(&con->rs);
        uint32_t printed = 0, waited = 0;
        while (rs_run_busy(con) && waited < CON_CAP_MS) {
            rs_run_step(con);
            for (size_t i = 0; i < rs_row_count(); i++) {
                uint8_t st = con->rs.st[i];
                if (rs_row_readable(i) && !(printed & (1u << i)) && (st == RS_V_VALUE || st == RS_V_NONE)) {
                    print_row(i);
                    printed |= 1u << i;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(CON_STEP_MS));
            waited += CON_STEP_MS;
        }
        size_t done, total, answered = 0;
        rs_progress(&con->rs, &done, &total);
        for (size_t i = 0; i < rs_row_count(); i++) {
            answered += con->rs.st[i] == RS_V_VALUE;
        }
        console_reply("settings: %u of %u rows answered%s%s%s", (unsigned)answered, (unsigned)total,
                      con->rs.stopped  ? " (stopped after 3 gets without an answer)"
                      : done < total   ? " (stopped: took too long)"
                                       : "",
                      con->why ? "; last problem: " : "",
                      con->why ? con->why : "");
    }
    __atomic_store_n(&con_running, false, __ATOMIC_RELEASE);
}

void mesh_rset_console_read(uint8_t const prefix[6]) {
    if (__atomic_load_n(&con_running, __ATOMIC_ACQUIRE)) {
        console_reply("settings: a read is still running");
        return;
    }
    if (!con) {
        con = lichen_alloc(meshcore_self(), sizeof(*con), LICHEN_MEM_BULK);
        if (!con) {
            console_reply("settings: not enough memory");
            return;
        }
    }
    rs_run_init(con, prefix);
    __atomic_store_n(&con_running, true, __ATOMIC_RELEASE);
    if (!lichen_task_start(meshcore_self(), "meshrset", con_task, NULL, CON_STACK, LICHEN_PRIO_JOB, NULL)) {
        __atomic_store_n(&con_running, false, __ATOMIC_RELEASE);
        console_reply("settings: no memory for the job");
        return;
    }
    console_reply("settings of %02x%02x%02x%02x%02x%02x: read only, one get at a time; rows print as they come "
                  "(fw mesh remote settings shows them again)",
                  prefix[0], prefix[1], prefix[2], prefix[3], prefix[4], prefix[5]);
}

void mesh_rset_console_show(void) {
    if (__atomic_load_n(&con_running, __ATOMIC_ACQUIRE)) {
        console_reply("settings: still reading, the rows print as they come");
        return;
    }
    if (!con) {
        console_reply("settings: none read yet: fw mesh remote <who> settings");
        return;
    }
    console_reply("settings of %02x%02x%02x%02x%02x%02x:", con->target[0], con->target[1], con->target[2],
                  con->target[3], con->target[4], con->target[5]);
    for (size_t i = 0; i < rs_row_count(); i++) {
        if (rs_row_readable(i)) {
            print_row(i);
        }
    }
}
