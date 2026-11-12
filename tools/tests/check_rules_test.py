#!/usr/bin/env python3
"""Host test of tools/check_rules.py (#287): each check fires on a fixture, stays quiet on the compliant twin, and the
baseline ratchet (new, worse, equal) works. Run with python3 -I."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import check_rules as cr  # noqa: E402


class Tree:
    def __init__(self, tc, files, baselines=None):
        self.tmp = tempfile.TemporaryDirectory()
        tc.addCleanup(self.tmp.cleanup)
        self.root = self.tmp.name
        for name, body in files.items():
            path = os.path.join(self.root, "firmware/components/x", name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(body)
        self.base = os.path.join(self.root, "tools")
        os.makedirs(self.base)
        for name, body in (baselines or {}).items():
            with open(os.path.join(self.base, name), "w") as f:
                f.write(body)

    def keys(self, check, allow=()):
        files = list(cr.tree_files(self.root))
        fn = {"cs-psram": cr.check_cs_psram, "task-class": cr.check_task_class, "lvgl-task": cr.check_lvgl_task,
              "task-delete": cr.check_task_delete, "log-level": lambda f: cr.check_log_level(f, list(allow))}[check]
        return sorted(cr.key_str(self.root, k) for k in fn(files))

    def run(self, check):
        return cr.run(self.root, self.base, [check], quiet=True)


CS_BAD = """\
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char EXT_RAM_BSS_ATTR big[4096];
static char small[8];
void bad(void) {
    portENTER_CRITICAL(&mux);
    memcpy(small, big, 8);
    portEXIT_CRITICAL(&mux);
}
void bad_alloc(void) {
    portENTER_CRITICAL(&mux);
    char *p = malloc(4);
    portEXIT_CRITICAL(&mux);
}
void bad_local(void) {
    char *buf = lichen_alloc(NULL, 100, LICHEN_MEM_BULK);
    taskENTER_CRITICAL(&mux);
    buf[0] = 1;
    taskEXIT_CRITICAL(&mux);
}
"""
CS_OK = """\
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char EXT_RAM_BSS_ATTR big[4096];
static char small[8];
void good(void) {
    char copy[8];
    memcpy(copy, big, 8); // PSRAM outside the section
    portENTER_CRITICAL(&mux);
    memcpy(small, copy, 8);
    portEXIT_CRITICAL(&mux);
}
void other(void) {
    char *buf = lichen_alloc(NULL, 100, LICHEN_MEM_BULK);
    char *p = 0;
    (void)buf;
    portENTER_CRITICAL(&mux);
    p = small; // a different function's local called buf is not PSRAM here
    portEXIT_CRITICAL(&mux);
}
"""


class CsPsram(unittest.TestCase):
    def test_fires(self):
        keys = Tree(self, {"a.c": CS_BAD}).keys("cs-psram")
        self.assertEqual(keys, ["firmware/components/x/a.c:bad:psram:big",
                                "firmware/components/x/a.c:bad_alloc:malloc",
                                "firmware/components/x/a.c:bad_local:psram:buf"])

    def test_quiet(self):
        self.assertEqual(Tree(self, {"a.c": CS_OK}).keys("cs-psram"), [])

    def test_comment_and_string_ignored(self):
        src = 'void f(void){ portENTER_CRITICAL(&m); /* malloc(1) */ log("free(x)"); portEXIT_CRITICAL(&m); }\n'
        self.assertEqual(Tree(self, {"a.c": src}).keys("cs-psram"), [])


LOG_BAD = 'void f(void) {\n    ESP_LOGE(TAG, "send failed, will retry (%d)", n);\n}\n' \
          'void g(void) { ESP_LOGE(TAG, "a, b"\n "dropped %s", x); }\n'
LOG_OK = 'void f(void) { ESP_LOGW(TAG, "will retry"); ESP_LOGE(TAG, "mount failed"); /* ESP_LOGE(TAG, "again") */ }\n'


class LogLevel(unittest.TestCase):
    def test_fires_including_comma_in_text_and_split_literal(self):
        keys = Tree(self, {"a.c": LOG_BAD}).keys("log-level")
        self.assertEqual(keys, ["firmware/components/x/a.c:f:retry", "firmware/components/x/a.c:g:dropped"])

    def test_quiet(self):
        self.assertEqual(Tree(self, {"a.c": LOG_OK}).keys("log-level"), [])

    def test_allow_list(self):
        keys = Tree(self, {"a.c": LOG_BAD}).keys("log-level", allow=[("x/a.c", "will retry")])
        self.assertEqual(keys, ["firmware/components/x/a.c:g:dropped"])


TASK_SRC = """\
static void ret_task(void *a) { for (;;) { if (a) return; } }
static void leak_task(void *a) { for (;;) { if (a) break; } }
static void inner_ok_task(void *a) { for (;;) { switch ((int)(intptr_t)a) { case 1: break; } } }
static void falls_task(void *a) { while (!stop) { work(); } }
static void flash_task(void *a) { for (;;) { helper(); } }
static void helper(void) { nvs_set_i32(h, "k", 1); }
static void noret_task(void *a) { vTaskDelay(1); lichen_sys_power_off_now(); }
static void fine_task(void *a) { for (;;) { vTaskDelay(1); } }
void start(void) {
    lichen_task_start_in(NULL, "a", ret_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "b", leak_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "c", inner_ok_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "d", falls_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "e", flash_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "f", flash_task, NULL, 4096, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "g", fine_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "h", missing_task, NULL, 4096, P, LICHEN_STACK_PSRAM, NULL);
    lichen_task_start_in(NULL, "i", ret_task, NULL, 4096, P, LICHEN_STACK_INTERNAL, NULL);
    lichen_task_start_in(NULL, "j", ret_task, NULL, 4096, P, some_variable, NULL);
}
"""


class TaskClass(unittest.TestCase):
    def test_all_rules(self):
        keys = Tree(self, {"a.c": TASK_SRC}).keys("task-class")
        p = "firmware/components/x/a.c:"
        self.assertEqual(keys, [p + "falls_task:falls-off-the-end", p + "flash_task:flash:nvs_set_i32",
                                p + "leak_task:breaks-out-of-loop", p + "missing_task:task-function-not-in-file",
                                p + "ret_task:class-not-a-literal", p + "ret_task:returns"])

    def test_noreturn_call_and_endless_loop_pass(self):
        src = TASK_SRC.split("void start")[0] + "void start(void) {\n" \
            "    lichen_task_start_in(NULL, \"n\", noret_task, NULL, 1, P, LICHEN_STACK_LP_RAM, NULL);\n" \
            "    lichen_task_start_in(NULL, \"f\", fine_task, NULL, 1, P, LICHEN_STACK_PSRAM, NULL);\n}\n"
        self.assertEqual(Tree(self, {"a.c": src}).keys("task-class"), [])


LV_SRC = """\
static void bad_task(void *a) { lv_label_set_text(l, "x"); }
static void locked_task(void *a) {
    if (lichen_sys_ui_lock(0)) { lv_obj_clean(o); lichen_sys_ui_unlock(); }
    lv_obj_del(o);
}
static void cb(void *a) { lv_obj_clean(a); }
static void ui_task(void *a) { lichen_ui_call(cb, a); }
static void raw_task(void *a) { lv_timer_handler(); }
void start(void) {
    lichen_task_start(NULL, "a", bad_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "b", locked_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "c", ui_task, NULL, 1, P, NULL);
    xTaskCreate(raw_task, "r", 1, NULL, 1, NULL);
}
"""


class LvglTask(unittest.TestCase):
    def test_fires_outside_the_lock_only(self):
        p = "firmware/components/x/a.c:"
        self.assertEqual(Tree(self, {"a.c": LV_SRC}).keys("lvgl-task"),
                         [p + "bad_task:lv_label_set_text", p + "locked_task:lv_obj_del",
                          p + "raw_task:lv_timer_handler"])


class Ratchet(unittest.TestCase):
    KEY = "firmware/components/x/a.c:f:retry"

    def test_new_fails(self):
        self.assertEqual(Tree(self, {"a.c": LOG_BAD}).run("log-level"), 2)

    def test_equal_baseline_passes_and_worse_fails(self):
        base = {"rules-baseline-log-level.txt": self.KEY + "\t1\nfirmware/components/x/a.c:g:dropped\t1\n"}
        self.assertEqual(Tree(self, {"a.c": LOG_BAD}, base).run("log-level"), 0)
        worse = LOG_BAD + 'void f2(void) {}\n'.replace("f2", "f") + 'void k(void){}\n'
        base = {"rules-baseline-log-level.txt": self.KEY + "\t1\n"}
        self.assertEqual(Tree(self, {"a.c": LOG_BAD}, base).run("log-level"), 1)  # g is new
        self.assertEqual(Tree(self, {"a.c": worse}, base).run("log-level"), 1)

    def test_shrunk_baseline_passes(self):
        base = {"rules-baseline-log-level.txt": self.KEY + "\t3\n"}
        self.assertEqual(Tree(self, {"a.c": LOG_OK}, base).run("log-level"), 0)


SELF_DEL = """\
static void del_task(void *a) { for (;;) { if (a) { vTaskDelete(NULL); } } }
static void end_task(void *a) { vTaskDelay(1); vTaskDelete(NULL); }
void start(void) {
    lichen_task_start_in(NULL, "a", del_task, NULL, 1, P, LICHEN_STACK_LP_RAM, NULL);
    lichen_task_start_in(NULL, "b", end_task, NULL, 1, P, LICHEN_STACK_PSRAM, NULL);
}
"""


class TaskClassSelfDelete(unittest.TestCase):
    def test_vtaskdelete_is_a_finding_and_not_a_noreturn(self):
        p = "firmware/components/x/a.c:"
        self.assertEqual(Tree(self, {"a.c": SELF_DEL}).keys("task-class"),
                         [p + "del_task:self-delete", p + "end_task:falls-off-the-end", p + "end_task:self-delete"])


TASK_DEL = """\
static void finish(void) { vTaskDelete( NULL ); }
static void del_task(void *a) { log(); vTaskDelete(NULL); }
static void cur_task(void *a) { vTaskDelete(xTaskGetCurrentTaskHandle()); }
static void via_task(void *a) { finish(); }
static void in_task(void *a) { vTaskDelete(0); }
static void ok_task(void *a) { vTaskDelete(other); return; }
static void raw_task(void *a) { vTaskDelete(NULL); }
void start(void) {
    lichen_task_start(NULL, "a", del_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "b", (TaskFunction_t)cur_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "c", &via_task, NULL, 1, P, NULL);
    lichen_task_start_in(NULL, "d", in_task, NULL, 1, P, LICHEN_STACK_INTERNAL, NULL);
    lichen_task_start(NULL, "e", ok_task, NULL, 1, P, NULL);
    lichen_task_start(NULL, "f", elsewhere_task, NULL, 1, P, NULL);
    xTaskCreate(raw_task, "g", 1, NULL, 1, NULL);
}
"""


class TaskDelete(unittest.TestCase):
    """#358: a lichen_task_start* task that deletes itself leaks its book slot; raw xTaskCreate tasks must delete."""

    def test_self_delete_in_entry_or_callee(self):
        p = "firmware/components/x/a.c:"
        self.assertEqual(Tree(self, {"a.c": TASK_DEL}).keys("task-delete"),
                         [p + "cur_task:self-delete-in-cur_task", p + "del_task:self-delete-in-del_task",
                          p + "in_task:self-delete-in-in_task", p + "via_task:self-delete-in-finish"])

    def test_returning_task_and_ratchet(self):
        ok = "static void t(void *a) { log(); }\nvoid s(void) { lichen_task_start(NULL, \"t\", t, NULL, 1, P, NULL); }\n"
        self.assertEqual(Tree(self, {"a.c": ok}).run("task-delete"), 0)
        self.assertEqual(Tree(self, {"a.c": TASK_DEL}).run("task-delete"), 4)


LV2 = """\
static void helper(void) { lv_obj_clean(o); }
static void locked_helper(void) { lv_obj_clean(o); }
static void calls_task(void *a) { helper(); if (lichen_sys_ui_lock(0)) { locked_helper(); lichen_sys_ui_unlock(); } }
static void cast_task(void *a) { lv_timer_handler(); }
static void amp_task(void *a) { lv_timer_handler(); }
void wrapper(void (*fn)(void *)) { lichen_task_start(NULL, "w", fn, NULL, 1, P, NULL); }
void start(void) {
    lichen_task_start(NULL, "a", calls_task, NULL, 1, P, NULL);
    xTaskCreate((TaskFunction_t)cast_task, "c", 1, NULL, 1, NULL);
    xTaskCreate(&amp_task, "d", 1, NULL, 1, NULL);
    xTaskCreate(elsewhere_task, "e", 1, NULL, 1, NULL);
}
"""


class LvglTaskMore(unittest.TestCase):
    def test_casts_callees_and_unresolved_entries(self):
        p = "firmware/components/x/a.c:"
        self.assertEqual(Tree(self, {"a.c": LV2}).keys("lvgl-task"),
                         [p + "amp_task:lv_timer_handler", p + "calls_task:lv_obj_clean",
                          p + "cast_task:lv_timer_handler", p + "elsewhere_task:entry-not-in-file",
                          p + "fn:entry-not-in-file"])


CS_EARLY = """\
void f(int x) {
    portENTER_CRITICAL(&mux);
    if (x) {
        portEXIT_CRITICAL(&mux);
        return;
    }
    char *p = malloc(4);
    portEXIT_CRITICAL(&mux);
}
"""


class CsEarlyExit(unittest.TestCase):
    def test_early_exit_in_a_branch_does_not_end_the_section(self):
        self.assertEqual(Tree(self, {"a.c": CS_EARLY}).keys("cs-psram"), ["firmware/components/x/a.c:f:malloc"])


LOG_MORE = """\
void f(void) {
    ESP_EARLY_LOGE(TAG, "will retry");
    ESP_DRAM_LOGE(DRAM_STR(TAG), DRAM_STR("dropped"));
    ESP_LOG_LEVEL(ESP_LOG_ERROR, TAG, "skipped %d", n);
    ESP_LOG_LEVEL(ESP_LOG_WARN, TAG, "skipped %d", n);
    ESP_LOGE(TAG, fmt_var, 1);
}
"""


class LogLevelMore(unittest.TestCase):
    def test_variants_and_nonliteral(self):
        cr.NONLITERAL.clear()
        keys = Tree(self, {"a.c": LOG_MORE}).keys("log-level")
        self.assertEqual(keys, ["firmware/components/x/a.c:f:dropped", "firmware/components/x/a.c:f:retry",
                                "firmware/components/x/a.c:f:skipped"])
        self.assertEqual(len(cr.NONLITERAL), 1)


class Counts(unittest.TestCase):
    def test_function_counts(self):
        t = Tree(self, {"a.c": LV_SRC, "b.h": "int x;\n"})
        c = cr.function_counts(list(cr.tree_files(t.root)))
        self.assertEqual(c[".c"][0], 1)
        self.assertGreater(c[".c"][1], 5)


if __name__ == "__main__":
    unittest.main()
