# The command splitter (console_cmd.c) is host-tested in host_tests/console_cmd_fw_test.c with this prefix
idf_component_register(SRCS "console.c" "console_cmd.c" "restart_log.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_uart app_update nvs_flash esp_system esp_timer heap freertos assets)
target_compile_definitions(${COMPONENT_LIB} PRIVATE "CONSOLE_CMD_PREFIX=\"fw \"")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_restart")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nvs_flash_erase")
