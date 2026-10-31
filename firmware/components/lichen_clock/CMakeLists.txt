# Which time to believe (#8): clock_check.c is pure and host-tested (host_tests/clock_check_test.c). lichen_clock.c
# holds the state (clock_is_set, last SNTP step); the build epoch is the commit time of the checkout, so the build is
# reproducible (unlike __DATE__). clock_local.c (local time to UTC, "fw clock set" parsing, #64) is pure and host-tested
# too (clock_local_test.c); clock_store.c keeps the source across a warm reset in Lichen's NVS (lichen_api), and
# "fw clock sync" restarts lwIP's SNTP client (lwip).
execute_process(
    COMMAND git log -1 --format=%ct
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE build_epoch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE build_epoch_result)
if(NOT build_epoch_result EQUAL 0 OR build_epoch STREQUAL "")
    message(FATAL_ERROR "git log -1 --format=%ct failed: the clock check needs the build epoch")
endif()

idf_component_register(SRCS "clock_check.c" "clock_local.c" "lichen_clock.c" "clock_cmd.c" "clock_store.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES fw_console esp_hw_support esp_timer log pthread lwip freertos lichen_api)
# lichen_clock is the only settimeofday() caller (#83): every other call lands in __wrap_settimeofday (lichen_clock.c),
# which logs the caller (log-only until the App API boundary work, #84, makes it refuse)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=settimeofday")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LICHEN_BUILD_EPOCH=${build_epoch}LL)
