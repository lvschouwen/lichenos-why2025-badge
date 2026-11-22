// The SD apps' export table (#519): the generated lichen_exports.c and the committed tools/export_table.txt. The
// generator and the gate (tools/gen_export_table.py --check) own "matches the headers today"; this pins what must hold
// of the table itself: sorted and unique, the two files agree, every name comes from a gate header (the manifest) or
// the allow list, no wrapped function is exported under its real address, and nothing the apps are barred from leaks.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ROOT
#error "ROOT (the repository root) is set by CMake"
#endif
static char const k_root[] = ROOT;

#define MAX_ROWS 2048
#define STR      96

typedef struct {
    char name[STR];
    char sym[STR];
    char origin[STR];
} row_t;

static row_t c_rows[MAX_ROWS], t_rows[MAX_ROWS];
static int   c_n, t_n;
static char  wraps[64][STR];
static int   wrap_n;
static char  manifest[MAX_ROWS][STR];
static int   manifest_n;
static char  allow[MAX_ROWS][STR];
static int   allow_n;
static int   fails;

static void check(int ok, char const *fmt, ...) {
    if (ok) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fails++;
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static FILE *open_rel(char const *rel) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", k_root, rel);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    return f;
}

static void copy(char *dst, char const *src, size_t n) {
    snprintf(dst, STR, "%.*s", (int)n, src);
}

// lines like:     {"name", (void *)symbol},
static void load_c(void) {
    FILE *f = open_rel("firmware/components/lichen_os/lichen_exports.c");
    char  line[256];
    while (fgets(line, sizeof(line), f)) {
        char const *q = strstr(line, "{\"");
        char const *c = strstr(line, "(void *)");
        if (!q || !c) {
            continue;
        }
        char const *e = strchr(q + 2, '"');
        char const *end = strchr(c + 8, '}');
        if (!e || !end || c_n >= MAX_ROWS) {
            continue;
        }
        copy(c_rows[c_n].name, q + 2, (size_t)(e - q - 2));
        copy(c_rows[c_n].sym, c + 8, (size_t)(end - c - 8));
        c_n++;
    }
    fclose(f);
}

static void load_table(void) {
    FILE *f = open_rel("tools/export_table.txt");
    char  line[512];
    while (fgets(line, sizeof(line), f)) {
        char *tok[4] = {0};
        line[strcspn(line, "\n")] = 0;
        int n = 0;
        for (char *p = strtok(line, "\t"); p && n < 4; p = strtok(NULL, "\t")) {
            tok[n++] = p;
        }
        if (n >= 2 && !strcmp(tok[0], "wrap") && wrap_n < 64) {
            copy(wraps[wrap_n++], tok[1], STR);
        } else if (n == 4 && !strcmp(tok[0], "export") && t_n < MAX_ROWS) {
            copy(t_rows[t_n].name, tok[1], STR);
            copy(t_rows[t_n].sym, tok[2], STR);
            copy(t_rows[t_n].origin, tok[3], STR);
            t_n++;
        }
    }
    fclose(f);
}

static void load_manifest(void) {
    FILE *f = open_rel("tools/gate_manifest.txt");
    char  line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || manifest_n >= MAX_ROWS) {
            continue;
        }
        copy(manifest[manifest_n++], line, strcspn(line, "\t\n"));
    }
    fclose(f);
}

// `name` or `name = target`: the app's name is the first word
static void load_allow(void) {
    FILE *f = open_rel("tools/export_allow.txt");
    char  line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        size_t n = strspn(line, " \t");
        size_t w = strcspn(line + n, " \t=\n");
        if (w && allow_n < MAX_ROWS) {
            copy(allow[allow_n++], line + n, w);
        }
    }
    fclose(f);
}

static int has(char const (*list)[STR], int n, char const *s) {
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i], s)) {
            return 1;
        }
    }
    return 0;
}

// Names an app is barred from by layers.conf (`forbid L6`) that must never be in the table under their own name
static char const *const barred[] = {"lichen_sys_ui_lock", "lichen_ctx_enter", "lichen_ctx_leave", "lichen_radio_set",
                                     "update_mode_start", "esp_restart", "abort", "lora_internal_send", "printf",
                                     "esp_rom_printf", "bench_scope_bind", "key_cause_bind_last_key", NULL};

static void check_order_and_agreement(void) {
    check(c_n > 100, "lichen_exports.c has only %d entries", c_n);
    check(c_n == t_n, "lichen_exports.c has %d entries, export_table.txt %d", c_n, t_n);
    for (int i = 1; i < c_n; i++) {
        check(strcmp(c_rows[i - 1].name, c_rows[i].name) < 0, "not sorted and unique at %s / %s", c_rows[i - 1].name,
              c_rows[i].name);
    }
    for (int i = 0; i < c_n && i < t_n; i++) {
        check(!strcmp(c_rows[i].name, t_rows[i].name) && !strcmp(c_rows[i].sym, t_rows[i].sym),
              "row %d differs: %s -> %s in the C file, %s -> %s in the table", i, c_rows[i].name, c_rows[i].sym,
              t_rows[i].name, t_rows[i].sym);
    }
}

static void check_origin_and_wraps(void) {
    for (int i = 0; i < t_n; i++) {
        row_t const *r = &t_rows[i];
        if (!strcmp(r->origin, "allow")) {
            check(has(allow, allow_n, r->name), "%s claims the allow list but is not in it", r->name);
        } else {
            check(has(manifest, manifest_n, r->name), "%s is exported from %s but the gate manifest lacks it", r->name,
                  r->origin);
        }
        // A wrapped function's real name is never the address in the table: the app gets the wrapper or a mapping
        check(!has(wraps, wrap_n, r->sym), "%s is exported under its real name although it is wrapped (--wrap=%s)",
              r->name, r->sym);
    }
    for (int i = 0; barred[i]; i++) {
        for (int j = 0; j < t_n; j++) {
            check(strcmp(t_rows[j].name, barred[i]) != 0, "%s must not be exported to apps", barred[i]);
        }
    }
    check(wrap_n > 0, "no --wrap found: the grep of the CMakeLists.txt files is broken");
}

int main(void) {
    load_c();
    load_table();
    load_manifest();
    load_allow();
    check_order_and_agreement();
    check_origin_and_wraps();
    printf("export_table_test: %d entries, %d wraps, %d failure(s)\n", c_n, wrap_n, fails);
    return fails ? 1 : 0;
}
