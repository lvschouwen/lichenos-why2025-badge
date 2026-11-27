# Keeps every crash (#74): core dump on flash, a text log on /storage, "fw crash". crash_log.c is pure, host-tested in
# host_tests/crash_log_test.c. pmp_cmd.c is "fw pmp" (#608), a developer-mode command, and pmp_fmt.c its pure decoder
# (host_tests/pmp_fmt_test.c), so a release build (LICHEN_DEV_COMMANDS off) leaves both out.
set(srcs "crash.c" "crash_log.c" "crash_twdt.c" "boot_count.c")
if(CONFIG_LICHEN_DEV_COMMANDS)
    list(APPEND srcs "pmp_cmd.c" "pmp_fmt.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES espcoredump esp_partition app_update bootloader_support esp_system esp_timer esp_rom nvs_flash
                                     storage fw_console riscv soc)
