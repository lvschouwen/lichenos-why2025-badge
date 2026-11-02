# Lichen's start-up steps (#66), below the shell so c6link can report its steps without depending on it (#84). Pure
# and host-tested: boot_steps.c (host_tests/boot_steps_test.c), boot_text.c (boot_text_test.c), log_clean.c
# (log_clean_test.c). boot_progress.c wraps boot_steps in a critical section for the badge; log_tap.c keeps the last
# console lines for the boot screen and "fw boot". factory_plan.c (factory_plan_test.c, #66) decides what a factory
# reset erases and runs the boot combo's state; factory_reset.c erases it at boot and is "fw factory-reset".
idf_component_register(SRCS "boot_steps.c" "boot_text.c" "boot_progress.c" "log_tap.c" "log_clean.c" "factory_plan.c" "factory_reset.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES lichen_api fw_console freertos heap nvs_flash fatfs storage vfs esp_partition wear_levelling)
