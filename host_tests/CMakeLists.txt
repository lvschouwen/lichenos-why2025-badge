cmake_minimum_required(VERSION 3.16)
project(host_tests)

# Set C standard
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

message(STATUS "Host test compiler: ${CMAKE_C_COMPILER}")
message(STATUS "Host test compiler ID: ${CMAKE_C_COMPILER_ID}")
message(STATUS "Host test compiler version: ${CMAKE_C_COMPILER_VERSION}")

# Enable testing
enable_testing()

# ASan + UBSan on every host test (#196): memory bugs in pure logic fail here, not on the badge.
# -DHOST_SANITIZE=OFF builds without them (e.g. under valgrind).
option(HOST_SANITIZE "Build host tests with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
if(HOST_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    # Vendored ed25519 (ref10, MeshCore submodule) left-shifts negative values by design; GCC defines the result
    file(GLOB ED25519_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../meshcore/lib/ed25519/*.c)
    set_source_files_properties(${ED25519_SRCS} PROPERTIES COMPILE_OPTIONS -fno-sanitize=shift-base)
endif()


add_executable(c6_image_test
    ${CMAKE_CURRENT_SOURCE_DIR}/c6_image_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_image.c
)

target_compile_options(c6_image_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(c6_image_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link)
add_test(NAME c6_image_test COMMAND c6_image_test)

add_executable(c6_boot_policy_test
    ${CMAKE_CURRENT_SOURCE_DIR}/c6_boot_policy_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_boot_policy.c
)

target_compile_options(c6_boot_policy_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(c6_boot_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link)
add_test(NAME c6_boot_policy_test COMMAND c6_boot_policy_test)

add_executable(c6_watch_test
    ${CMAKE_CURRENT_SOURCE_DIR}/c6_watch_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_watch.c
)

target_compile_options(c6_watch_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(c6_watch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link)
add_test(NAME c6_watch_test COMMAND c6_watch_test)

# c6link's hooks to the layers above it (#223): an unset hook does its default
add_executable(c6_hooks_test
    ${CMAKE_CURRENT_SOURCE_DIR}/c6_hooks_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_hooks.c
)
target_compile_options(c6_hooks_test PRIVATE -Wall -Wextra -Werror)
target_include_directories(c6_hooks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link)
add_test(NAME c6_hooks_test COMMAND c6_hooks_test)

add_executable(ota_proto_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_proto_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/sha256.c
)

target_include_directories(ota_proto_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)

target_compile_options(ota_proto_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME ota_proto_test COMMAND ota_proto_test)

add_executable(ota_autooff_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_autooff_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_autooff.c
)
target_include_directories(ota_autooff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)
target_compile_options(ota_autooff_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_autooff_test COMMAND ota_autooff_test)

add_executable(ota_pieces_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_pieces_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_pieces.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/sha256.c
)

target_include_directories(ota_pieces_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)

target_compile_options(ota_pieces_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME ota_pieces_test COMMAND ota_pieces_test)

add_executable(psram_pattern_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_pattern_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_pattern.c
)
target_include_directories(psram_pattern_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
target_compile_options(psram_pattern_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_pattern_test COMMAND psram_pattern_test)

add_executable(psram_rotation_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_rotation_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_rotation.c
)
target_include_directories(psram_rotation_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
target_compile_options(psram_rotation_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_rotation_test COMMAND psram_rotation_test)

add_executable(psram_speed_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_speed_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_speed_logic.c
)
target_include_directories(psram_speed_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
target_compile_options(psram_speed_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_speed_test COMMAND psram_speed_test)

add_executable(psram_tuning_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_tuning_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_tuning.c
)
target_include_directories(psram_tuning_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
target_compile_options(psram_tuning_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_tuning_test COMMAND psram_tuning_test)

add_executable(psram_heap_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_heap_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_heap.c
)
target_include_directories(psram_heap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
target_compile_options(psram_heap_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_heap_test COMMAND psram_heap_test)

add_executable(psram_dma_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_dma_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_pattern.c
)
target_include_directories(psram_dma_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
target_compile_options(psram_dma_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_dma_test COMMAND psram_dma_test)

add_executable(ui_perf_stats_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_perf_stats_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/ui_perf_stats.c
)
target_include_directories(ui_perf_stats_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(ui_perf_stats_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ui_perf_stats_test COMMAND ui_perf_stats_test)

# The "fw mem" heap line with totals and percentages (#205)
add_executable(mem_fmt_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mem_fmt_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console/mem_fmt.c
)
target_include_directories(mem_fmt_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(mem_fmt_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mem_fmt_test COMMAND mem_fmt_test)

# When the DMA heap's low-water mark fell (#256): the step detection behind "fw mem"'s dma_low lines
add_executable(dma_low_test
    ${CMAKE_CURRENT_SOURCE_DIR}/dma_low_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/dma_low.c
)
target_include_directories(dma_low_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os)
target_compile_options(dma_low_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME dma_low_test COMMAND dma_low_test)

add_executable(bench_calc_test
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_calc_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/bench_calc.c
)
target_include_directories(bench_calc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(bench_calc_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME bench_calc_test COMMAND bench_calc_test)

add_executable(stress_calc_test
    ${CMAKE_CURRENT_SOURCE_DIR}/stress_calc_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/stress_calc.c
)
target_include_directories(stress_calc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(stress_calc_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME stress_calc_test COMMAND stress_calc_test)

add_executable(attention_policy_test
    ${CMAKE_CURRENT_SOURCE_DIR}/attention_policy_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/attention_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/backlight_level.c
)
target_include_directories(attention_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(attention_policy_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME attention_policy_test COMMAND attention_policy_test)

add_executable(idle_policy_test
    ${CMAKE_CURRENT_SOURCE_DIR}/idle_policy_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/idle_policy.c
)
target_include_directories(idle_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(idle_policy_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME idle_policy_test COMMAND idle_policy_test)

add_executable(restart_frame_test
    ${CMAKE_CURRENT_SOURCE_DIR}/restart_frame_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/restart_frame.c
)
target_include_directories(restart_frame_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(restart_frame_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME restart_frame_test COMMAND restart_frame_test)

add_executable(draw_stack_test
    ${CMAKE_CURRENT_SOURCE_DIR}/draw_stack_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/draw_stack.c
)
target_include_directories(draw_stack_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(draw_stack_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME draw_stack_test COMMAND draw_stack_test)

add_executable(backlight_cmd_test
    ${CMAKE_CURRENT_SOURCE_DIR}/backlight_cmd_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main/backlight_cmd.c
)

target_include_directories(backlight_cmd_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main)

target_compile_options(backlight_cmd_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME backlight_cmd_test COMMAND backlight_cmd_test)

add_executable(backlight_level_test
    ${CMAKE_CURRENT_SOURCE_DIR}/backlight_level_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/backlight_level.c
)
target_include_directories(backlight_level_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board)
target_compile_options(backlight_level_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME backlight_level_test COMMAND backlight_level_test)

add_executable(r16_hold_test
    ${CMAKE_CURRENT_SOURCE_DIR}/r16_hold_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/r16_hold.c
)
target_include_directories(r16_hold_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board)
target_compile_options(r16_hold_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME r16_hold_test COMMAND r16_hold_test)

add_executable(uart_lend_test
    ${CMAKE_CURRENT_SOURCE_DIR}/uart_lend_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/uart_lend.c
)
target_include_directories(uart_lend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board)
target_compile_options(uart_lend_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME uart_lend_test COMMAND uart_lend_test)

add_executable(display_rot_test
    ${CMAKE_CURRENT_SOURCE_DIR}/display_rot_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/display_rot.c
)
target_include_directories(display_rot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board)
target_compile_options(display_rot_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME display_rot_test COMMAND display_rot_test)

add_executable(console_cmd_test
    ${CMAKE_CURRENT_SOURCE_DIR}/console_cmd_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console/console_cmd.c
)

target_compile_options(console_cmd_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(console_cmd_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
add_test(NAME console_cmd_test COMMAND console_cmd_test)

add_executable(lora_proto_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_proto_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora/lora_proto.c
)

target_include_directories(lora_proto_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora)

target_compile_options(lora_proto_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME lora_proto_test COMMAND lora_proto_test)

# The settings registry core (#308): parse, range, format, find, the set/serve rules (UNKNOWN, GUARDED, read-back)
add_executable(reg_test ${CMAKE_CURRENT_SOURCE_DIR}/reg_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg/reg.c)
target_include_directories(reg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg)
target_compile_options(reg_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME reg_test COMMAND reg_test)

# The C6's settings registry (#309): its table against reg_ids.h, backlight duty <-> %, the SX1262 read-back decoders
add_executable(c6_reg_test
    ${CMAKE_CURRENT_SOURCE_DIR}/c6_reg_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main/reg_c6.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main/lora_readback.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main/backlight_cmd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg/reg.c
)
target_include_directories(c6_reg_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora
)
target_compile_options(c6_reg_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(c6_reg_test PRIVATE m)
add_test(NAME c6_reg_test COMMAND c6_reg_test)

# The P4's registry service (#310): the real P4 table, fw set rules (radio keys ERR guarded, no setter), the C6 keys'
# proxy cache (seq, late and unknown-type answers, age, stale after a reset), the old-C6 path, the applied-vs-requested
# match rules and the once-per-key mismatch
add_executable(reg_service_test ${CMAKE_CURRENT_SOURCE_DIR}/reg_service_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg/reg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_cmd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_proxy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_psram.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_sensors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_misc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_settings.c
    ${CMAKE_CURRENT_SOURCE_DIR}/reg_stubs_psram.c
    ${CMAKE_CURRENT_SOURCE_DIR}/reg_stubs_display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/reg_stubs_sensors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/reg_stubs_misc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/reg_stubs_settings.c)
target_include_directories(reg_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board)
target_compile_options(reg_service_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME reg_service_test COMMAND reg_service_test)

# ---- registry domain 0x11 psram.* (#316): this domain's agent adds its own test below and touches no other block -------
# (the rows are reg_p4_psram.c; reg_service_test above already links every row file and pins the split)
add_executable(psram_regs_test
    ${CMAKE_CURRENT_SOURCE_DIR}/psram_regs_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_regs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard/psram_tuning.c)
target_include_directories(psram_regs_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg)
target_compile_options(psram_regs_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME psram_regs_test COMMAND psram_regs_test)
# reg_p4_psram.c and its stub take psram_reg_get's prototype from psram_reg.h, so a signature change fails the build
target_include_directories(reg_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/psram_guard)
# ---- end of the psram block ------------------------------------------------------------------------------

# ---- registry domain 0x12 disp.* (#317): this domain's agent adds its own test below and touches no other block -------
# (the rows are reg_p4_display.c; reg_service_test above already links every row file and pins the split)
add_executable(display_timing_test
    ${CMAKE_CURRENT_SOURCE_DIR}/display_timing_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/display_timing.c
)
target_include_directories(display_timing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board)
target_compile_options(display_timing_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME display_timing_test COMMAND display_timing_test)
# ---- end of the display block ------------------------------------------------------------------------------

# ---- registry domain 0x13 sens.* (#318): this domain's agent adds its own test below and touches no other block -------
# (the rows are reg_p4_sensors.c; reg_service_test above already links every row file and pins the split)
add_executable(reg_sensors_test ${CMAKE_CURRENT_SOURCE_DIR}/reg_sensors_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg/reg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_p4_sensors.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sensors/sensors_cfg.c)
target_include_directories(reg_sensors_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sensors)
target_compile_options(reg_sensors_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(reg_sensors_test PRIVATE m)
add_test(NAME reg_sensors_test COMMAND reg_sensors_test)
# ---- end of the sensors block ------------------------------------------------------------------------------

# ---- registry domain 0x14 haptics.*, led.*, tz, hold.* (#319): this domain's agent adds its own test below and touches no other block -------
# (the rows are reg_p4_misc.c; reg_service_test above already links every row file and pins the split)
target_include_directories(reg_service_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_haptics
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_led ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock)
# The pure helpers of #319 (hp_duty_pct, led_pad_text, clock_tz_label) are tested in haptics_pattern_test, led_pattern_test
# and clock_local_test; the getters are ESP calls, checked on the bench
# ---- end of the misc block ------------------------------------------------------------------------------

# ---- registry domain 0x15 settings.* (#320): this domain's agent adds its own test below and touches no other block -------
# (the rows are reg_p4_settings.c; reg_service_test above already links every row file and pins the split)
# ---- end of the settings block ------------------------------------------------------------------------------

# The Wi-Fi registry keys (#311): the fixed country NL as the driver reports it, the regulatory log text, bandwidth
# mapping, the fw wifi tx|bw|ps alias replies and the wifi.* views
add_executable(wifi_reg_test ${CMAKE_CURRENT_SOURCE_DIR}/wifi_reg_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg/reg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_cmd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/reg_wifi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_regdom.c)
target_include_directories(wifi_reg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net)
target_compile_options(wifi_reg_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_reg_test COMMAND wifi_reg_test)

# The LoRa task's registry request queue (#310): order, one entry per id, the newest seq, a full queue
add_executable(lora_regq_test ${CMAKE_CURRENT_SOURCE_DIR}/lora_regq_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/lora_regq.c)
target_include_directories(lora_regq_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora)
target_compile_options(lora_regq_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lora_regq_test COMMAND lora_regq_test)

add_executable(meshcore_hdr_test
    ${CMAKE_CURRENT_SOURCE_DIR}/meshcore_hdr_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/meshcore_hdr.c
)

target_include_directories(meshcore_hdr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)

# Radio noise can pass the CRC: prove the parser never reads past a packet
target_compile_options(meshcore_hdr_test PRIVATE
    -Wall
    -Wextra
    -Werror
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
)
target_link_options(meshcore_hdr_test PRIVATE -fsanitize=address,undefined)

add_test(NAME meshcore_hdr_test COMMAND meshcore_hdr_test)

add_executable(lora_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_link_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/lora_link.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora/lora_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/thirdparty/tomlc17.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/why_io_libc.c
)

# why_io.h uses clang's nullability qualifiers
target_compile_definitions(lora_link_test PRIVATE _Nullable=)

target_include_directories(lora_link_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared
)

# The shipped lora.toml must hold the built-in defaults
target_compile_definitions(lora_link_test PRIVATE SKEL_LORA_TOML="${CMAKE_CURRENT_SOURCE_DIR}/../firmware/lora.toml")

# Only our sources get the strict warnings; tomlc17 is third-party
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_link_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/lora_link.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror"
)

target_link_libraries(lora_link_test PRIVATE m)

add_test(NAME lora_link_test COMMAND lora_link_test)

add_executable(lora_irq_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_irq_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main/lora_irq.c
)

target_include_directories(lora_irq_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../c6/main)

target_compile_options(lora_irq_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME lora_irq_test COMMAND lora_irq_test)

add_executable(console_cmd_fw_test
    ${CMAKE_CURRENT_SOURCE_DIR}/console_cmd_fw_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console/console_cmd.c
)

# The lean firmware (#61) builds the same splitter with its own prefix
target_compile_definitions(console_cmd_fw_test PRIVATE "CONSOLE_CMD_PREFIX=\"fw \"")

target_compile_options(console_cmd_fw_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(console_cmd_fw_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
add_test(NAME console_cmd_fw_test COMMAND console_cmd_fw_test)

# One console_exec run's output and its secret lines (#123)
add_executable(console_capture_test
    ${CMAKE_CURRENT_SOURCE_DIR}/console_capture_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console/console_capture.c
)
target_include_directories(console_capture_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(console_capture_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME console_capture_test COMMAND console_capture_test)

# The assets image format (#22); the packer builds the test image from host_tests/vectors/assets/
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(assets_fmt_test
    ${CMAKE_CURRENT_SOURCE_DIR}/assets_fmt_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/assets/assets_fmt.c
)

target_include_directories(assets_fmt_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/assets)

target_compile_options(assets_fmt_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME assets_pack COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/pack_assets.py
    ${CMAKE_CURRENT_SOURCE_DIR}/vectors/assets ${CMAKE_CURRENT_BINARY_DIR}/assets_test.bin)
add_test(NAME assets_fmt_test COMMAND assets_fmt_test ${CMAKE_CURRENT_BINARY_DIR}/assets_test.bin)
set_tests_properties(assets_fmt_test PROPERTIES DEPENDS assets_pack)

# The flash host's serial logger picks the badge's CH340 after a re-enumeration (#115)
add_test(NAME serial_log_test COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/serial_log_test.py)

# Lichen's generated fonts cover the text the badge shows (#16)
add_executable(font_cover_test ${CMAKE_CURRENT_SOURCE_DIR}/font_cover_test.c)
target_compile_options(font_cover_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME font_cover_test COMMAND font_cover_test ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/assets/fonts)

add_executable(c6_paths_test
    ${CMAKE_CURRENT_SOURCE_DIR}/c6_paths_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_paths.c
)

target_include_directories(c6_paths_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link)

target_compile_options(c6_paths_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME c6_paths_test COMMAND c6_paths_test)

add_executable(ota_path_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_path_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_path.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_proto.c
)

target_include_directories(ota_path_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/storage
)

target_compile_options(ota_path_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME ota_path_test COMMAND ota_path_test)

add_executable(ota_valid_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_valid_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_valid.c
)

target_include_directories(ota_valid_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)

target_compile_options(ota_valid_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME ota_valid_test COMMAND ota_valid_test)

add_executable(restart_gate_test
    ${CMAKE_CURRENT_SOURCE_DIR}/restart_gate_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_valid.c
)

target_include_directories(restart_gate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)

target_compile_options(restart_gate_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME restart_gate_test COMMAND restart_gate_test)

add_executable(kbd_map_test
    ${CMAKE_CURRENT_SOURCE_DIR}/kbd_map_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/kbd_map.c
)

target_include_directories(kbd_map_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board
)

target_compile_options(kbd_map_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

add_test(NAME kbd_map_test COMMAND kbd_map_test)

add_executable(tca8418_mask_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tca8418_mask_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/esp_tca8418/tca8418_mask.c
)
target_include_directories(tca8418_mask_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/esp_tca8418/include
)
target_compile_options(tca8418_mask_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME tca8418_mask_test COMMAND tca8418_mask_test)

# MeshCore's protocol code with rweather's Crypto and our Arduino shim, as the lean firmware builds it (#61)
set(MESHCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../meshcore)
set(CRYPTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/arduinolibs/libraries/Crypto)
file(GLOB ED25519_SOURCES ${MESHCORE_DIR}/lib/ed25519/*.c)
# The stored identity's check at load (#80): MeshCore's Ed25519 code alone, built as is (third party: no -Werror)
add_library(ed25519_host STATIC ${ED25519_SOURCES})
target_include_directories(ed25519_host SYSTEM PUBLIC ${MESHCORE_DIR}/lib/ed25519)
target_compile_options(ed25519_host PRIVATE -w)
add_executable(mesh_ident_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_ident_test.c
                               ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_ident.c)
target_include_directories(mesh_ident_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_ident_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mesh_ident_test ed25519_host)
add_test(NAME mesh_ident_test COMMAND mesh_ident_test)
add_library(meshcore_host STATIC
    ${MESHCORE_DIR}/src/Dispatcher.cpp
    ${MESHCORE_DIR}/src/Mesh.cpp
    ${MESHCORE_DIR}/src/Packet.cpp
    ${MESHCORE_DIR}/src/Identity.cpp
    ${MESHCORE_DIR}/src/Utils.cpp
    ${MESHCORE_DIR}/src/helpers/BaseChatMesh.cpp
    ${MESHCORE_DIR}/src/helpers/AdvertDataHelpers.cpp
    ${MESHCORE_DIR}/src/helpers/TxtDataHelpers.cpp
    ${MESHCORE_DIR}/src/helpers/StaticPoolPacketManager.cpp
    ${ED25519_SOURCES}
    ${CRYPTO_DIR}/AES128.cpp
    ${CRYPTO_DIR}/AESCommon.cpp
    ${CRYPTO_DIR}/SHA256.cpp
    ${CRYPTO_DIR}/SHA512.cpp
    ${CRYPTO_DIR}/Ed25519.cpp
    ${CRYPTO_DIR}/Curve25519.cpp
    ${CRYPTO_DIR}/BigNumberUtil.cpp
    ${CRYPTO_DIR}/Crypto.cpp
    ${CRYPTO_DIR}/Hash.cpp
    ${CRYPTO_DIR}/BlockCipher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim/arduino_shim.cpp
)
target_include_directories(meshcore_host SYSTEM PUBLIC
    ${MESHCORE_DIR}/src
    ${MESHCORE_DIR}/lib/ed25519
    ${CRYPTO_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/base64/src
)
# As the badge's MeshCore companion builds (meshcore/variants/why2025_badge); they size classes, so PUBLIC
target_compile_definitions(meshcore_host PUBLIC MAX_CONTACTS=350 MAX_GROUP_CHANNELS=40)
# Third-party code: built as is, without our strict warnings
target_compile_options(meshcore_host PRIVATE -w)

add_executable(meshcore_decode_test ${CMAKE_CURRENT_SOURCE_DIR}/meshcore_decode_test.cpp)
target_link_libraries(meshcore_decode_test PRIVATE meshcore_host)
target_compile_options(meshcore_decode_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME meshcore_decode_test COMMAND meshcore_decode_test ${CMAKE_CURRENT_SOURCE_DIR}/vectors/meshcore_rx.txt)

# Not a test: regenerates vectors/meshcore_rx.txt (synthetic packets, see the file's header)
add_executable(make_vectors ${CMAKE_CURRENT_SOURCE_DIR}/make_vectors.cpp)
target_link_libraries(make_vectors PRIVATE meshcore_host)
target_compile_options(make_vectors PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(mesh_store_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_store_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
)
target_include_directories(mesh_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_store_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_store_test COMMAND mesh_store_test)

add_executable(mesh_hist_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_hist_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_hist.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_hist_boot.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_hist_crypto.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_hist_selftest.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
)
target_include_directories(mesh_hist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
# The journal's encryption (#98) uses rweather's Crypto (SHA-256, AES-128) as MeshCore builds it
target_link_libraries(mesh_hist_test PRIVATE meshcore_host)
target_compile_options(mesh_hist_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_hist_test COMMAND mesh_hist_test)

add_executable(lora_airtime_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_airtime_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/lora_airtime.c
)
target_include_directories(lora_airtime_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(lora_airtime_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lora_airtime_test PRIVATE m)
add_test(NAME lora_airtime_test COMMAND lora_airtime_test)

add_executable(mesh_format_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_format_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
)
target_include_directories(mesh_format_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_format_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_format_test COMMAND mesh_format_test)

add_executable(mesh_contact_fmt_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_contact_fmt_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_contact_fmt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
)
target_include_directories(mesh_contact_fmt_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_contact_fmt_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_contact_fmt_test COMMAND mesh_contact_fmt_test)

add_executable(mesh_frame_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_frame_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
)
target_include_directories(mesh_frame_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_frame_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_frame_test COMMAND mesh_frame_test)

add_executable(mesh_outbox_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_outbox_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_outbox.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
)
target_include_directories(mesh_outbox_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_outbox_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_outbox_test COMMAND mesh_outbox_test)

add_executable(mesh_trace_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_trace_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
)
target_include_directories(mesh_trace_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_trace_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_trace_test COMMAND mesh_trace_test)

add_executable(mesh_remote_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_remote_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_remote.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_rset.c
)
target_include_directories(mesh_remote_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_remote_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mesh_remote_test PRIVATE m)
add_test(NAME mesh_remote_test COMMAND mesh_remote_test)

add_executable(mesh_rset_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_rset_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_rset.c
)
target_include_directories(mesh_rset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_rset_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mesh_rset_test PRIVATE m)
add_test(NAME mesh_rset_test COMMAND mesh_rset_test)

add_executable(mesh_pathhash_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_pathhash_test.c)
target_include_directories(mesh_pathhash_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_pathhash_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_pathhash_test COMMAND mesh_pathhash_test)

add_executable(radio_presets_test
    ${CMAKE_CURRENT_SOURCE_DIR}/radio_presets_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_presets.c
)
target_include_directories(radio_presets_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(radio_presets_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(radio_presets_test PRIVATE m)
add_test(NAME radio_presets_test COMMAND radio_presets_test)

add_executable(list_filter_test
    ${CMAKE_CURRENT_SOURCE_DIR}/list_filter_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/list_filter.c
)
target_include_directories(list_filter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(list_filter_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME list_filter_test COMMAND list_filter_test)

add_executable(radio_filter_test
    ${CMAKE_CURRENT_SOURCE_DIR}/radio_filter_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_filter.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_presets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/list_filter.c
)
target_include_directories(radio_filter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(radio_filter_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(radio_filter_test PRIVATE m)
add_test(NAME radio_filter_test COMMAND radio_filter_test)

add_executable(radio_confirm_test
    ${CMAKE_CURRENT_SOURCE_DIR}/radio_confirm_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_confirm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_presets.c
)
target_include_directories(radio_confirm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(radio_confirm_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(radio_confirm_test PRIVATE m)
add_test(NAME radio_confirm_test COMMAND radio_confirm_test)

add_executable(channel_input_test
    ${CMAKE_CURRENT_SOURCE_DIR}/channel_input_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/channel_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
)
target_include_directories(channel_input_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(channel_input_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME channel_input_test COMMAND channel_input_test)

add_executable(mesh_share_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_share_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_share.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_contact_fmt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
)
target_include_directories(mesh_share_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_share_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_share_test COMMAND mesh_share_test)

add_executable(mesh_chat_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_chat_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_chat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/channel_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/list_filter.c
)
target_include_directories(mesh_chat_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(mesh_chat_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_chat_test COMMAND mesh_chat_test)

add_executable(mesh_chat_target_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_chat_target_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_chat_target.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/channel_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/list_filter.c
)
target_include_directories(mesh_chat_target_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(mesh_chat_target_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_chat_target_test COMMAND mesh_chat_target_test)

add_executable(mesh_notify_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_notify_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_notify.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
)
target_include_directories(mesh_notify_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_notify_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_notify_test COMMAND mesh_notify_test)

add_executable(mesh_tile_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_tile_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_tile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
)
target_include_directories(mesh_tile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_tile_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_tile_test COMMAND mesh_tile_test)

add_executable(advert_limit_test ${CMAKE_CURRENT_SOURCE_DIR}/advert_limit_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/advert_limit.c)
target_include_directories(advert_limit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(advert_limit_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME advert_limit_test COMMAND advert_limit_test)

add_executable(mesh_blobq_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_blobq_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_blobq.c)
target_include_directories(mesh_blobq_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_blobq_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_blobq_test COMMAND mesh_blobq_test)

add_executable(mesh_advert_cfg_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_advert_cfg_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_advert_cfg.c)
target_include_directories(mesh_advert_cfg_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_advert_cfg_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_advert_cfg_test COMMAND mesh_advert_cfg_test)

add_executable(mesh_scope_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_scope_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_scope.c)
target_include_directories(mesh_scope_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_scope_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_scope_test COMMAND mesh_scope_test)

add_executable(mesh_grid_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_grid_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_grid.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_advert_cfg.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/home_layout.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/tile_order.c)
target_include_directories(mesh_grid_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(mesh_grid_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_grid_test COMMAND mesh_grid_test)
add_executable(mesh_run_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_run_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_run.c)
target_include_directories(mesh_run_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_run_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_run_test COMMAND mesh_run_test)
add_executable(mesh_health_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_health_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_health.c)
target_include_directories(mesh_health_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_health_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_health_test COMMAND mesh_health_test)
add_executable(mesh_airlog_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_airlog_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_airlog.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_health.c)
target_include_directories(mesh_airlog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_airlog_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_airlog_test COMMAND mesh_airlog_test)
add_executable(mesh_signal_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_signal_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_signal.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_health.c)
target_include_directories(mesh_signal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_signal_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_signal_test COMMAND mesh_signal_test)
add_executable(mesh_discover_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_discover_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_discover.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_health.c)
target_include_directories(mesh_discover_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_discover_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_discover_test COMMAND mesh_discover_test)
add_executable(mesh_radar_test ${CMAKE_CURRENT_SOURCE_DIR}/mesh_radar_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_radar.c)
target_include_directories(mesh_radar_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_radar_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mesh_radar_test PRIVATE m)
add_test(NAME mesh_radar_test COMMAND mesh_radar_test)

add_executable(crash_log_test
    ${CMAKE_CURRENT_SOURCE_DIR}/crash_log_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/crash/crash_log.c
)
target_include_directories(crash_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/crash)
target_compile_options(crash_log_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME crash_log_test COMMAND crash_log_test)

add_executable(restart_log_test
    ${CMAKE_CURRENT_SOURCE_DIR}/restart_log_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console/restart_log.c
)
target_include_directories(restart_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(restart_log_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME restart_log_test COMMAND restart_log_test)

add_executable(boot_steps_test
    ${CMAKE_CURRENT_SOURCE_DIR}/boot_steps_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/boot_steps.c
)
target_include_directories(boot_steps_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot)
target_compile_options(boot_steps_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME boot_steps_test COMMAND boot_steps_test)

add_executable(log_clean_test
    ${CMAKE_CURRENT_SOURCE_DIR}/log_clean_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/log_clean.c
)
target_include_directories(log_clean_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot)
target_compile_options(log_clean_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME log_clean_test COMMAND log_clean_test)

add_executable(fs_shim_test
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_shim_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim/fs_shim.cpp
)
target_include_directories(fs_shim_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim)
target_compile_options(fs_shim_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME fs_shim_test COMMAND fs_shim_test)

add_executable(cayenne_lpp_test ${CMAKE_CURRENT_SOURCE_DIR}/cayenne_lpp_test.cpp)
target_include_directories(cayenne_lpp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim)
target_compile_options(cayenne_lpp_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME cayenne_lpp_test COMMAND cayenne_lpp_test)

# MeshCore's companion (examples/companion_radio) on the badge target (#65). Its own copy of the MeshCore sources:
# the limits below size MeshCore's classes, so they must match in every file of one program.
add_library(meshcore_companion_host STATIC
    ${MESHCORE_DIR}/src/Dispatcher.cpp
    ${MESHCORE_DIR}/src/Mesh.cpp
    ${MESHCORE_DIR}/src/Packet.cpp
    ${MESHCORE_DIR}/src/Identity.cpp
    ${MESHCORE_DIR}/src/Utils.cpp
    ${MESHCORE_DIR}/src/helpers/BaseChatMesh.cpp
    ${MESHCORE_DIR}/src/helpers/AdvertDataHelpers.cpp
    ${MESHCORE_DIR}/src/helpers/TxtDataHelpers.cpp
    ${MESHCORE_DIR}/src/helpers/StaticPoolPacketManager.cpp
    ${MESHCORE_DIR}/src/helpers/IdentityStore.cpp
    ${MESHCORE_DIR}/src/helpers/ConfigSerializer.cpp
    ${MESHCORE_DIR}/src/helpers/DynamicConfigSerializer.cpp
    ${MESHCORE_DIR}/src/helpers/CommonRadioPrefs.cpp
    ${MESHCORE_DIR}/src/helpers/TransportKeyStore.cpp
    ${MESHCORE_DIR}/src/helpers/RegionMap.cpp
    ${MESHCORE_DIR}/examples/companion_radio/MyMesh.cpp
    ${MESHCORE_DIR}/examples/companion_radio/DataStore.cpp
    ${ED25519_SOURCES}
    ${CRYPTO_DIR}/AES128.cpp
    ${CRYPTO_DIR}/AESCommon.cpp
    ${CRYPTO_DIR}/SHA256.cpp
    ${CRYPTO_DIR}/SHA512.cpp
    ${CRYPTO_DIR}/Ed25519.cpp
    ${CRYPTO_DIR}/Curve25519.cpp
    ${CRYPTO_DIR}/BigNumberUtil.cpp
    ${CRYPTO_DIR}/Crypto.cpp
    ${CRYPTO_DIR}/Hash.cpp
    ${CRYPTO_DIR}/BlockCipher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim/arduino_shim.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim/fs_shim.cpp
)
# stubs first: shared/arduino_shim/nvs_flash.h only declares nvs_flash_erase (the firmware renames and defines it), so
# it must not win over the host stub's inline definition; which header won depended on the generator
target_include_directories(meshcore_companion_host SYSTEM BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${MESHCORE_DIR}/src
    ${MESHCORE_DIR}/examples/companion_radio
    ${MESHCORE_DIR}/lib/ed25519
    ${CRYPTO_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/base64/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/companion
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock
)
target_compile_definitions(meshcore_companion_host PUBLIC MAX_CONTACTS=1000 MAX_GROUP_CHANNELS=40
                           OFFLINE_QUEUE_SIZE=256)
# ESP32 picks the fs::FS file system in IdentityStore.h (and SPIFFS in DataStore); only the files that include it get
# it, because elsewhere it would switch rweather Crypto to the ESP32's hardware AES
set(COMPANION_ESP32_SRCS ${MESHCORE_DIR}/src/helpers/IdentityStore.cpp ${MESHCORE_DIR}/src/helpers/RegionMap.cpp
    ${MESHCORE_DIR}/src/helpers/TransportKeyStore.cpp ${MESHCORE_DIR}/examples/companion_radio/MyMesh.cpp
    ${MESHCORE_DIR}/examples/companion_radio/DataStore.cpp ${CMAKE_CURRENT_SOURCE_DIR}/companion_test.cpp)
set_source_files_properties(${COMPANION_ESP32_SRCS} ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_link.cpp PROPERTIES COMPILE_DEFINITIONS ESP32)
target_compile_options(meshcore_companion_host PRIVATE -w)

# target.h's BadgeRtc asks lichen_clock whether the clock is set
execute_process(COMMAND git log -1 --format=%ct WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE HOST_BUILD_EPOCH OUTPUT_STRIP_TRAILING_WHITESPACE)
set(LICHEN_CLOCK_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock/clock_check.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock/lichen_clock.c)
set_source_files_properties(${LICHEN_CLOCK_SRCS} PROPERTIES COMPILE_DEFINITIONS LICHEN_BUILD_EPOCH=${HOST_BUILD_EPOCH}LL)
add_executable(companion_test ${CMAKE_CURRENT_SOURCE_DIR}/companion_test.cpp ${LICHEN_CLOCK_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_presets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_outbox.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/channel_input.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/advert_limit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_blobq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_advert_cfg.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_scope.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_remote.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_rset.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_contact_fmt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_link.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_ident.c)
target_link_libraries(companion_test PRIVATE meshcore_companion_host)
target_include_directories(companion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(companion_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
add_test(NAME companion_test COMMAND companion_test ${CMAKE_CURRENT_SOURCE_DIR}/vectors/meshcore_rx.txt)

add_executable(lora_tx_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_tx_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/lora_tx.c
)
target_include_directories(lora_tx_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora
                           ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora)
target_compile_options(lora_tx_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lora_tx_test COMMAND lora_tx_test)

add_executable(lora_owner_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_owner_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/lora_owner.c
)
target_include_directories(lora_owner_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora)
target_compile_options(lora_owner_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lora_owner_test COMMAND lora_owner_test)

add_executable(lora_switch_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_switch_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/lora_switch.c
)
target_include_directories(lora_switch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora)
target_compile_options(lora_switch_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lora_switch_test COMMAND lora_switch_test)

add_executable(nav_test
    ${CMAKE_CURRENT_SOURCE_DIR}/nav_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/nav.c
)
target_include_directories(nav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(nav_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME nav_test COMMAND nav_test)

add_executable(screen_ids_test ${CMAKE_CURRENT_SOURCE_DIR}/screen_ids_test.c)
target_include_directories(screen_ids_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(screen_ids_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME screen_ids_test COMMAND screen_ids_test)

add_executable(mc_first_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mc_first_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mc_first.c
)
target_include_directories(mc_first_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mc_first_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mc_first_test COMMAND mc_first_test)

add_executable(ui_keys_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_keys_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/ui_keys.c
)
target_include_directories(ui_keys_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(ui_keys_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ui_keys_test COMMAND ui_keys_test)

add_executable(ui_overflow_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_overflow_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/ui_overflow.c
)
target_include_directories(ui_overflow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(ui_overflow_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ui_overflow_test COMMAND ui_overflow_test)

add_executable(setup_state_test
    ${CMAKE_CURRENT_SOURCE_DIR}/setup_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/setup_state.c
)
target_include_directories(setup_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(setup_state_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME setup_state_test COMMAND setup_state_test)

add_executable(app_ns_test
    ${CMAKE_CURRENT_SOURCE_DIR}/app_ns_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/app_ns.c
)
target_include_directories(app_ns_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(app_ns_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME app_ns_test COMMAND app_ns_test)

add_executable(legacy_blob_test
    ${CMAKE_CURRENT_SOURCE_DIR}/legacy_blob_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/legacy_blob.c
)
target_include_directories(legacy_blob_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(legacy_blob_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME legacy_blob_test COMMAND legacy_blob_test)

# lichen_task_start's bookkeeping and lichen_alloc's per-app accounting (#181)
foreach(book task_book mem_book)
    add_executable(${book}_test
        ${CMAKE_CURRENT_SOURCE_DIR}/${book}_test.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/${book}.c
    )
    target_include_directories(${book}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
    target_compile_options(${book}_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${book}_test COMMAND ${book}_test)
endforeach()

add_executable(badge_table_test
    ${CMAKE_CURRENT_SOURCE_DIR}/badge_table_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/badge_table.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/app_ns.c
)
target_include_directories(badge_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(badge_table_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME badge_table_test COMMAND badge_table_test)

add_executable(boot_text_test
    ${CMAKE_CURRENT_SOURCE_DIR}/boot_text_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/boot_text.c
)
target_include_directories(boot_text_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot)
target_compile_options(boot_text_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME boot_text_test COMMAND boot_text_test)

add_executable(update_status_test
    ${CMAKE_CURRENT_SOURCE_DIR}/update_status_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/update_status.c
)
target_include_directories(update_status_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(update_status_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME update_status_test COMMAND update_status_test)

add_executable(update_mode_test
    ${CMAKE_CURRENT_SOURCE_DIR}/update_mode_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/update_mode.c
)
target_include_directories(update_mode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(update_mode_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME update_mode_test COMMAND update_mode_test)

add_executable(home_layout_test
    ${CMAKE_CURRENT_SOURCE_DIR}/home_layout_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/home_layout.c
)
target_include_directories(home_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(home_layout_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME home_layout_test COMMAND home_layout_test)

add_executable(remote_ctl_test
    ${CMAKE_CURRENT_SOURCE_DIR}/remote_ctl_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/remote_ctl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/hold_exit.c
)
target_include_directories(remote_ctl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(remote_ctl_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME remote_ctl_test COMMAND remote_ctl_test)

add_executable(hold_exit_test
    ${CMAKE_CURRENT_SOURCE_DIR}/hold_exit_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/hold_exit.c
)
target_include_directories(hold_exit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(hold_exit_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME hold_exit_test COMMAND hold_exit_test)

add_executable(lock_combo_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lock_combo_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/lock_combo.c
)
target_include_directories(lock_combo_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(lock_combo_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lock_combo_test COMMAND lock_combo_test)

add_executable(saver_layout_test
    ${CMAKE_CURRENT_SOURCE_DIR}/saver_layout_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/saver_layout.c
)
target_include_directories(saver_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(saver_layout_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME saver_layout_test COMMAND saver_layout_test)

add_executable(station_clock_test
    ${CMAKE_CURRENT_SOURCE_DIR}/station_clock_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/station_clock.c
)
target_include_directories(station_clock_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(station_clock_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(station_clock_test PRIVATE m)
add_test(NAME station_clock_test COMMAND station_clock_test)

add_executable(dialog_countdown_test
    ${CMAKE_CURRENT_SOURCE_DIR}/dialog_countdown_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/dialog_countdown.c
)
target_include_directories(dialog_countdown_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(dialog_countdown_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME dialog_countdown_test COMMAND dialog_countdown_test)

add_executable(key_matrix_test
    ${CMAKE_CURRENT_SOURCE_DIR}/key_matrix_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/key_matrix.c
)
target_include_directories(key_matrix_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board
)
target_compile_options(key_matrix_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME key_matrix_test COMMAND key_matrix_test)

add_executable(shortcut_test
    ${CMAKE_CURRENT_SOURCE_DIR}/shortcut_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/shortcut.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/ui_keys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/key_matrix.c
)
target_include_directories(shortcut_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board
)
target_compile_options(shortcut_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME shortcut_test COMMAND shortcut_test)

add_executable(calc_eval_test
    ${CMAKE_CURRENT_SOURCE_DIR}/calc_eval_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/calc_eval.c
)
target_include_directories(calc_eval_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_options(calc_eval_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME calc_eval_test COMMAND calc_eval_test)

add_executable(calc_units_test
    ${CMAKE_CURRENT_SOURCE_DIR}/calc_units_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/calc_units.c
)
target_include_directories(calc_units_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_options(calc_units_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(calc_units_test PRIVATE m)
add_test(NAME calc_units_test COMMAND calc_units_test)

add_executable(timer_calc_test
    ${CMAKE_CURRENT_SOURCE_DIR}/timer_calc_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/timer_calc.c
)
target_include_directories(timer_calc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_options(timer_calc_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME timer_calc_test COMMAND timer_calc_test)

add_executable(tz_table_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tz_table_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/tz_table.c
)
target_include_directories(tz_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(tz_table_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME tz_table_test COMMAND tz_table_test)

add_executable(clock_check_test
    ${CMAKE_CURRENT_SOURCE_DIR}/clock_check_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock/clock_check.c
)
target_include_directories(clock_check_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock)
target_compile_options(clock_check_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME clock_check_test COMMAND clock_check_test)

add_executable(clock_local_test
    ${CMAKE_CURRENT_SOURCE_DIR}/clock_local_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock/clock_local.c
)
target_include_directories(clock_local_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock)
target_compile_options(clock_local_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME clock_local_test COMMAND clock_local_test)

# The shell's view of the clock, Wi-Fi and boot progress goes through lichen_sys_pure_view.c (#244): every host test that
# builds one of those shell files links it with the pure code behind it.
set(SYS_VIEW_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/lichen_sys_pure_view.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock/clock_check.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock/clock_local.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_saved.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_analyser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/boot_text.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/boot_steps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/log_clean.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c)
set(SYS_VIEW_INCS
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_clock ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_mdns ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)

add_executable(lichen_sys_pure_view_test ${CMAKE_CURRENT_SOURCE_DIR}/lichen_sys_pure_view_test.c ${SYS_VIEW_SRCS})
target_include_directories(lichen_sys_pure_view_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(lichen_sys_pure_view_test PRIVATE ${SYS_VIEW_INCS})
target_compile_options(lichen_sys_pure_view_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lichen_sys_pure_view_test COMMAND lichen_sys_pure_view_test)

add_executable(time_fields_test
    ${CMAKE_CURRENT_SOURCE_DIR}/time_fields_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/time_fields.c
    ${SYS_VIEW_SRCS}
)
target_include_directories(time_fields_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(time_fields_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${SYS_VIEW_INCS})
target_compile_options(time_fields_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME time_fields_test COMMAND time_fields_test)

add_executable(wifi_list_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_list_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_saved.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(wifi_list_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(wifi_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(wifi_list_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_list_test COMMAND wifi_list_test)

add_executable(link_state_test
    ${CMAKE_CURRENT_SOURCE_DIR}/link_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/link_state.c
    ${SYS_VIEW_SRCS}
)
target_include_directories(link_state_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(link_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${SYS_VIEW_INCS})
target_compile_options(link_state_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME link_state_test COMMAND link_state_test)

add_executable(lora_activity_test ${CMAKE_CURRENT_SOURCE_DIR}/lora_activity_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/lora_activity.c)
target_include_directories(lora_activity_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(lora_activity_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lora_activity_test COMMAND lora_activity_test)

add_executable(wifi_cred_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_cred_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_saved.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/settings_secret.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/settings_secret_keys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(wifi_cred_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(wifi_cred_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(wifi_cred_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_cred_test COMMAND wifi_cred_test)

add_executable(wifi_saved_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_saved_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_saved.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(wifi_saved_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(wifi_saved_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(wifi_saved_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_saved_test COMMAND wifi_saved_test)

add_executable(wifi_analyser_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_analyser_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_analyser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_saved.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(wifi_analyser_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(wifi_analyser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(wifi_analyser_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_analyser_test COMMAND wifi_analyser_test)

add_executable(wifi_join_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_join_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net/wifi_join.c
)
target_include_directories(wifi_join_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net)
target_compile_options(wifi_join_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_join_test COMMAND wifi_join_test)

add_executable(ui_text_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_text_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/ui_text.c
)
target_include_directories(ui_text_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(ui_text_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ui_text_test COMMAND ui_text_test)

add_executable(scroll_anchor_test
    ${CMAKE_CURRENT_SOURCE_DIR}/scroll_anchor_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/scroll_anchor.c
)
target_include_directories(scroll_anchor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(scroll_anchor_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME scroll_anchor_test COMMAND scroll_anchor_test)

add_executable(radio_state_test
    ${CMAKE_CURRENT_SOURCE_DIR}/radio_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/radio_state.c
)
target_include_directories(radio_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(radio_state_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME radio_state_test COMMAND radio_state_test)

add_executable(shot_blend_test
    ${CMAKE_CURRENT_SOURCE_DIR}/shot_blend_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/shot_blend.c
)
target_include_directories(shot_blend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(shot_blend_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME shot_blend_test COMMAND shot_blend_test)

add_executable(log_ring_test
    ${CMAKE_CURRENT_SOURCE_DIR}/log_ring_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/log_ring.c
)
target_include_directories(log_ring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(log_ring_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME log_ring_test COMMAND log_ring_test)

add_executable(console_model_test
    ${CMAKE_CURRENT_SOURCE_DIR}/console_model_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/console_model.c
)
target_include_directories(console_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(console_model_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME console_model_test COMMAND console_model_test)

add_executable(ota_bmp_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_bmp_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_bmp.c
)
target_include_directories(ota_bmp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)
target_compile_options(ota_bmp_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_bmp_test COMMAND ota_bmp_test)

add_executable(ota_rle_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_rle_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_rle.c
)
target_include_directories(ota_rle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)
target_compile_options(ota_rle_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_rle_test COMMAND ota_rle_test)

add_executable(hosted_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/hosted_link_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/mcu/eh_host_mcu_transport/src/eh_host_mcu_transport_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/features/eh_host_feat_rpc/src/eh_host_feat_rpc_slice.c
)
target_include_directories(hosted_link_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/mcu/eh_host_mcu_transport/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/features/eh_host_feat_rpc/src)
target_compile_options(hosted_link_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME hosted_link_test COMMAND hosted_link_test)

# The fixed Wi-Fi frame pools of the esp_hosted SDIO host (#260)
add_executable(frame_pool_test
    ${CMAKE_CURRENT_SOURCE_DIR}/frame_pool_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/mcu/eh_host_mcu_transport/src/eh_frame_pool_core.c
)
target_include_directories(frame_pool_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/mcu/eh_host_mcu_transport/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../esp_hosted/host/compat/include)
target_compile_options(frame_pool_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME frame_pool_test COMMAND frame_pool_test)

add_executable(sd_policy_test
    ${CMAKE_CURRENT_SOURCE_DIR}/sd_policy_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard/sd_policy.c
)
target_include_directories(sd_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard)
target_compile_options(sd_policy_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME sd_policy_test COMMAND sd_policy_test)

add_executable(sd_bounce_test
    ${CMAKE_CURRENT_SOURCE_DIR}/sd_bounce_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard/sd_bounce.c
)
target_include_directories(sd_bounce_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard)
target_compile_options(sd_bounce_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME sd_bounce_test COMMAND sd_bounce_test)

add_executable(fs_path_test
    ${CMAKE_CURRENT_SOURCE_DIR}/fs_path_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/fs_path.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/app_ns.c
)
target_include_directories(fs_path_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(fs_path_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME fs_path_test COMMAND fs_path_test)

# The per-app table slot lookup behind lichen_fs_app_dir (#272)
add_executable(app_slot_test
    ${CMAKE_CURRENT_SOURCE_DIR}/app_slot_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/app_slot.c
)
target_include_directories(app_slot_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os)
target_compile_options(app_slot_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME app_slot_test COMMAND app_slot_test)

add_executable(sd_settings_model_test ${CMAKE_CURRENT_SOURCE_DIR}/sd_settings_model_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/sd_settings_model.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/lichen_sys_pure.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard/sd_policy.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/factory_plan.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_autooff.c)
target_include_directories(sd_settings_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)
target_compile_options(sd_settings_model_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME sd_settings_model_test COMMAND sd_settings_model_test)

# The gate's storage / update / factory calls over the components' pure functions (#227)
add_executable(lichen_sys_pure_test ${CMAKE_CURRENT_SOURCE_DIR}/lichen_sys_pure_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/lichen_sys_pure.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard/sd_policy.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/factory_plan.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_autooff.c)
target_include_directories(lichen_sys_pure_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sdcard ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/storage)
target_compile_options(lichen_sys_pure_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lichen_sys_pure_test COMMAND lichen_sys_pure_test)

add_executable(files_model_test ${CMAKE_CURRENT_SOURCE_DIR}/files_model_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/files_model.c)
target_include_directories(files_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(files_model_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME files_model_test COMMAND files_model_test)

add_executable(files_ops_test ${CMAKE_CURRENT_SOURCE_DIR}/files_ops_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/files_ops.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/files_model.c)
target_include_directories(files_ops_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_definitions(files_ops_test PRIVATE FILES_HOST_TEST)
target_compile_options(files_ops_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME files_ops_test COMMAND files_ops_test)

add_executable(weather_series_test
    ${CMAKE_CURRENT_SOURCE_DIR}/weather_series_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/weather_series.c
)
target_include_directories(weather_series_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_options(weather_series_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(weather_series_test PRIVATE m)
add_test(NAME weather_series_test COMMAND weather_series_test)
add_executable(qr_payload_test ${CMAKE_CURRENT_SOURCE_DIR}/qr_payload_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/qr_payload.c)
target_include_directories(qr_payload_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(qr_payload_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME qr_payload_test COMMAND qr_payload_test)

add_executable(notes_model_test ${CMAKE_CURRENT_SOURCE_DIR}/notes_model_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/notes_model.c)
target_include_directories(notes_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_options(notes_model_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notes_model_test COMMAND notes_model_test)

add_executable(notes_store_test ${CMAKE_CURRENT_SOURCE_DIR}/notes_store_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/notes_store.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/notes_model.c)
target_include_directories(notes_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_definitions(notes_store_test PRIVATE NOTES_HOST_TEST)
target_compile_options(notes_store_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notes_store_test COMMAND notes_store_test)

add_executable(sensors_format_test
    ${CMAKE_CURRENT_SOURCE_DIR}/sensors_format_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sensors/sensors_format.c
)
target_include_directories(sensors_format_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/sensors)
target_compile_options(sensors_format_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sensors_format_test PRIVATE m)
add_test(NAME sensors_format_test COMMAND sensors_format_test)

add_executable(settings_backup_test ${CMAKE_CURRENT_SOURCE_DIR}/settings_backup_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/settings_backup.c)
target_include_directories(settings_backup_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(settings_backup_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME settings_backup_test COMMAND settings_backup_test)

add_executable(crashlog_fmt_test ${CMAKE_CURRENT_SOURCE_DIR}/crashlog_fmt_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/crashlog_fmt.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/crash/crash_log.c)
target_include_directories(crashlog_fmt_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/crash)
target_compile_options(crashlog_fmt_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME crashlog_fmt_test COMMAND crashlog_fmt_test)

add_executable(notify_ring_test ${CMAKE_CURRENT_SOURCE_DIR}/notify_ring_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify/notify_ring.c)
target_include_directories(notify_ring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(notify_ring_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notify_ring_test COMMAND notify_ring_test)

add_executable(notify_rate_test ${CMAKE_CURRENT_SOURCE_DIR}/notify_rate_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify/notify_rate.c)
target_include_directories(notify_rate_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify)
target_compile_options(notify_rate_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notify_rate_test COMMAND notify_rate_test)

add_executable(notify_mute_test ${CMAKE_CURRENT_SOURCE_DIR}/notify_mute_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify/notify_mute.c)
target_include_directories(notify_mute_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify)
target_compile_options(notify_mute_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notify_mute_test COMMAND notify_mute_test)

add_executable(led_pattern_test ${CMAKE_CURRENT_SOURCE_DIR}/led_pattern_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_led/led_pattern.c)
target_include_directories(led_pattern_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_led)
target_compile_options(led_pattern_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME led_pattern_test COMMAND led_pattern_test)

add_executable(haptics_pattern_test ${CMAKE_CURRENT_SOURCE_DIR}/haptics_pattern_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_haptics/haptics_pattern.c)
target_include_directories(haptics_pattern_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_haptics)
target_compile_options(haptics_pattern_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME haptics_pattern_test COMMAND haptics_pattern_test)

add_executable(led_setting_test ${CMAKE_CURRENT_SOURCE_DIR}/led_setting_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_led/led_setting.c)
target_include_directories(led_setting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_led ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(led_setting_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME led_setting_test COMMAND led_setting_test)

add_executable(haptics_setting_test ${CMAKE_CURRENT_SOURCE_DIR}/haptics_setting_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_haptics/haptics_setting.c)
target_include_directories(haptics_setting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_haptics ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(haptics_setting_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME haptics_setting_test COMMAND haptics_setting_test)

add_executable(notify_store_test ${CMAKE_CURRENT_SOURCE_DIR}/notify_store_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify/notify_store.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify/notify_mute.c)
target_include_directories(notify_store_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_notify ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(notify_store_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notify_store_test COMMAND notify_store_test)

add_executable(notify_view_test ${CMAKE_CURRENT_SOURCE_DIR}/notify_view_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/notify_view.c)
target_include_directories(notify_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(notify_view_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notify_view_test COMMAND notify_view_test)

add_executable(sys_notice_rules_test ${CMAKE_CURRENT_SOURCE_DIR}/sys_notice_rules_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/sys_notice_rules.c)
target_include_directories(sys_notice_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os)
target_compile_options(sys_notice_rules_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME sys_notice_rules_test COMMAND sys_notice_rules_test)

add_executable(thermal_rules_test ${CMAKE_CURRENT_SOURCE_DIR}/thermal_rules_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/thermal_rules.c)
target_include_directories(thermal_rules_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(thermal_rules_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME thermal_rules_test COMMAND thermal_rules_test)

add_executable(factory_plan_test ${CMAKE_CURRENT_SOURCE_DIR}/factory_plan_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/factory_plan.c)
target_include_directories(factory_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot)
target_compile_options(factory_plan_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME factory_plan_test COMMAND factory_plan_test)

# The settings' secrets class (#235): out of the backup table and the factory reset's keep list
add_executable(settings_secret_test ${CMAKE_CURRENT_SOURCE_DIR}/settings_secret_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/settings_secret_keys.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/settings_backup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot/factory_plan.c)
target_include_directories(settings_secret_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_net
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_boot)
target_compile_options(settings_secret_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME settings_secret_test COMMAND settings_secret_test)

# The notification service's state and feedback pushes to the drivers below it (#245)
set(NPT_FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components)
add_executable(notify_push_test ${CMAKE_CURRENT_SOURCE_DIR}/notify_push_test.c ${NPT_FW}/lichen_notify/notify_push.c)
target_include_directories(notify_push_test PRIVATE ${NPT_FW}/lichen_notify ${NPT_FW}/lichen_api ${NPT_FW}/fw_console)
target_compile_options(notify_push_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME notify_push_test COMMAND notify_push_test)

# The update server's bound settings and Wi-Fi switch, with their unbound defaults (#245)
add_executable(ota_os_test ${CMAKE_CURRENT_SOURCE_DIR}/ota_os_test.c ${NPT_FW}/ota/ota_os.c)
target_include_directories(ota_os_test PRIVATE ${NPT_FW}/ota)
target_compile_options(ota_os_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_os_test COMMAND ota_os_test)

# lichen_os's wiring below the gate (#245): the LED's and the motor's notify hooks, the lower services' settings
add_executable(os_bind_test ${CMAKE_CURRENT_SOURCE_DIR}/os_bind_test.c ${NPT_FW}/lichen_os/os_bind.c ${NPT_FW}/ota/ota_os.c)
target_include_directories(os_bind_test PRIVATE ${NPT_FW}/lichen_os ${NPT_FW}/lichen_api ${NPT_FW}/lichen_notify
                           ${NPT_FW}/lichen_boot ${NPT_FW}/lichen_haptics ${NPT_FW}/lichen_led ${NPT_FW}/lichen_clock
                           ${NPT_FW}/ota ${NPT_FW}/fw_console)
target_compile_options(os_bind_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME os_bind_test COMMAND os_bind_test)

# settings_store.c over the NVS stub (#235): the plain calls refuse a secret, settings_bind_net's store keeps them apart
set(SST_FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components)
add_executable(settings_store_test ${CMAKE_CURRENT_SOURCE_DIR}/settings_store_test.c
    ${SST_FW}/lichen_os/settings_store.c ${SST_FW}/lichen_os/settings_secret.c ${SST_FW}/lichen_os/settings_secret_keys.c
    ${SST_FW}/lichen_net/wifi_list.c ${SST_FW}/lichen_net/wifi_saved.c ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c)
target_include_directories(settings_store_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs/settings_store
                           ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(settings_store_test PRIVATE ${SST_FW}/lichen_os ${SST_FW}/lichen_api ${SST_FW}/lichen_net ${SST_FW}/fw_console
                           ${SST_FW}/board ${SST_FW}/sensors ${SST_FW}/lichen_haptics ${SST_FW}/lichen_led ${SST_FW}/lichen_notify)
target_compile_options(settings_store_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME settings_store_test COMMAND settings_store_test)

add_executable(release_notes_test
    ${CMAKE_CURRENT_SOURCE_DIR}/release_notes_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell/release_notes.c
)
target_include_directories(release_notes_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_shell)
target_compile_options(release_notes_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME release_notes_test COMMAND release_notes_test)

add_executable(nvs_image_test ${CMAKE_CURRENT_SOURCE_DIR}/nvs_image_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_nvs/nvs_image.c)
target_include_directories(nvs_image_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_nvs)
target_compile_options(nvs_image_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME nvs_image_test COMMAND nvs_image_test)

# The UI service's call queue, hand-over, lock-owner bookkeeping and hit table (#225), threaded
find_package(Threads REQUIRED)
add_executable(ui_call_queue_test ${CMAKE_CURRENT_SOURCE_DIR}/ui_call_queue_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/ui_call_queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/ui_lock_owner.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os/ui_lock_hits.c)
target_include_directories(ui_call_queue_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_os)
set_target_properties(ui_call_queue_test PROPERTIES C_STANDARD 11)
target_compile_options(ui_call_queue_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ui_call_queue_test PRIVATE Threads::Threads)
add_test(NAME ui_call_queue_test COMMAND ui_call_queue_test)

# The raw key hook table (#251): remove waits for a hook call in flight, threaded
add_executable(raw_hooks_test ${CMAKE_CURRENT_SOURCE_DIR}/raw_hooks_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board/raw_hooks.c)
target_include_directories(raw_hooks_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/board
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
set_target_properties(raw_hooks_test PROPERTIES C_STANDARD 11)
target_compile_options(raw_hooks_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(raw_hooks_test PRIVATE Threads::Threads)
add_test(NAME raw_hooks_test COMMAND raw_hooks_test)

add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    DEPENDS attention_policy_test mesh_hist_test assets_fmt_test font_cover_test c6_image_test ota_proto_test ota_pieces_test backlight_cmd_test backlight_level_test uart_lend_test console_cmd_test lora_proto_test reg_test meshcore_hdr_test lora_link_test lora_irq_test console_cmd_fw_test c6_paths_test ota_path_test ota_valid_test restart_gate_test kbd_map_test tca8418_mask_test meshcore_decode_test mesh_store_test lora_airtime_test mesh_format_test mesh_frame_test mesh_outbox_test mesh_trace_test mesh_remote_test mesh_rset_test radio_presets_test mesh_pathhash_test list_filter_test radio_filter_test radio_confirm_test channel_input_test mesh_chat_test mesh_tile_test mesh_notify_test crash_log_test restart_log_test boot_steps_test r16_hold_test log_clean_test fs_shim_test cayenne_lpp_test companion_test lora_tx_test lora_owner_test nav_test screen_ids_test home_layout_test badge_table_test hold_exit_test remote_ctl_test key_matrix_test tz_table_test clock_check_test clock_local_test time_fields_test wifi_list_test wifi_cred_test wifi_saved_test wifi_analyser_test wifi_join_test setup_state_test mc_first_test ui_keys_test ui_overflow_test boot_text_test update_status_test update_mode_test link_state_test scroll_anchor_test ui_text_test shot_blend_test ota_bmp_test ota_rle_test log_ring_test console_model_test psram_pattern_test psram_rotation_test psram_tuning_test psram_speed_test psram_heap_test ui_perf_stats_test mem_fmt_test dma_low_test bench_calc_test stress_calc_test idle_policy_test lock_combo_test saver_layout_test station_clock_test c6_watch_test dialog_countdown_test hosted_link_test sd_policy_test fs_path_test app_slot_test display_rot_test shortcut_test psram_dma_test sd_bounce_test calc_eval_test calc_units_test timer_calc_test
    sd_settings_model_test
    lichen_sys_pure_test
    lichen_sys_pure_view_test
    files_model_test
    files_ops_test
    radio_state_test
    ui_call_queue_test
    raw_hooks_test
    mesh_contact_fmt_test
    advert_limit_test
    mesh_blobq_test
    mesh_advert_cfg_test
    mesh_scope_test
    mesh_share_test
    mesh_grid_test
    mesh_run_test
    mesh_health_test
    mesh_airlog_test
    mesh_signal_test
    mesh_discover_test
    mesh_radar_test
    sensors_format_test
    weather_series_test
    thermal_rules_test
    crashlog_fmt_test
    settings_backup_test
    lora_activity_test
    notify_ring_test
    notify_rate_test
    notify_mute_test
    notify_store_test
    led_setting_test
    haptics_setting_test
    led_pattern_test
    haptics_pattern_test
    notify_view_test
    sys_notice_rules_test
    mesh_ident_test
    ota_autooff_test
    factory_plan_test
    settings_secret_test
    settings_store_test
    notify_push_test
    ota_os_test
    os_bind_test
    release_notes_test
    qr_payload_test
    notes_model_test
    notes_store_test
    mesh_chat_target_test
    nvs_image_test
    reg_service_test
    lora_regq_test
    COMMENT "Running all host tests"
)

add_executable(mdns_name_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mdns_name_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_mdns/mdns_name.c
)
target_include_directories(mdns_name_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_mdns)
target_compile_options(mdns_name_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mdns_name_test COMMAND mdns_name_test)

add_executable(mesh_telem_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_telem_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_telem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_remote.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_rset.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_frame.c
)
target_include_directories(mesh_telem_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_telem_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(mesh_telem_test PRIVATE m)
add_test(NAME mesh_telem_test COMMAND mesh_telem_test)

# LoRa spectrum app (#161): sweep plan, graph scale, peak hold, texts
add_executable(lora_spectrum_test
    ${CMAKE_CURRENT_SOURCE_DIR}/lora_spectrum_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps/lora_spectrum.c
)
target_include_directories(lora_spectrum_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_apps)
target_compile_options(lora_spectrum_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lora_spectrum_test COMMAND lora_spectrum_test)

# A grid's saved display order (#189): merge, parse, encode, move
add_executable(tile_order_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tile_order_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/tile_order.c
)
target_include_directories(tile_order_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(tile_order_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME tile_order_test COMMAND tile_order_test)

# tools/elf_archive.sh: ELF archive for crash decoding (#194)
add_test(NAME elf_archive_test COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/elf_archive_test.sh)
add_test(NAME badge_usage_test COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/badge_usage_test.sh)
add_test(NAME badge_log_test COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/badge_log_test.sh)
add_test(NAME guard_onair_test COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/guard_onair_test.sh)

# Battery model (#11): V to percent, warn/off with dwell, MAX17048 decode, ADC divider
add_executable(battery_model_test
    ${CMAKE_CURRENT_SOURCE_DIR}/battery_model_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/battery/battery_model.c
)
target_include_directories(battery_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/battery)
target_compile_options(battery_model_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME battery_model_test COMMAND battery_model_test)

# Repeater status history (#168): the poll schedule (off by default, cap, stagger, backoff), the ring, graphs, setting
add_executable(mesh_rhist_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_rhist_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_rhist.c
)
target_include_directories(mesh_rhist_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_rhist_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_rhist_test COMMAND mesh_rhist_test)
# Notification sounds (#67): tone patterns, volume curve, gating, rate limit, sample generator
add_executable(audio_pattern_test
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_pattern_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_audio/audio_pattern.c
)
target_include_directories(audio_pattern_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_audio)
target_compile_options(audio_pattern_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME audio_pattern_test COMMAND audio_pattern_test)


# tools/check_standard.py: the code and resource standard gate's baseline compare, ratchet and size deltas (#202)
add_test(NAME check_standard_test COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/check_standard_test.py)
# NVS encryption (#116): the boot policy (the burned-key path never erases) and the OTA slots' image mark
add_executable(nvs_boot_policy_test ${CMAKE_CURRENT_SOURCE_DIR}/nvs_boot_policy_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_nvs/nvs_boot_policy.c)
target_include_directories(nvs_boot_policy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_nvs)
target_compile_options(nvs_boot_policy_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME nvs_boot_policy_test COMMAND nvs_boot_policy_test)
add_executable(nvs_mark_test ${CMAKE_CURRENT_SOURCE_DIR}/nvs_mark_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_nvs/nvs_mark.c)
target_include_directories(nvs_mark_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_nvs)
target_compile_options(nvs_mark_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME nvs_mark_test COMMAND nvs_mark_test)

# 802.15.4 scanner protocol (#172): messages, MAC header parser on captured-style frames, dedupe, the old-C6 probe
add_executable(i154_proto_test
    ${CMAKE_CURRENT_SOURCE_DIR}/i154_proto_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/i154/i154_proto.c
)
target_include_directories(i154_proto_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared/i154)
target_compile_options(i154_proto_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME i154_proto_test COMMAND i154_proto_test)

# Two-chip contract (#234): peer-data ids, CONFIG bytes, IDENT compare
add_executable(two_chip_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/two_chip_contract_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora/lora_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/i154/i154_proto.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_boot_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link/c6_paths.c
)
target_include_directories(two_chip_contract_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/lora
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/i154
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/reg
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/c6link
)
target_compile_options(two_chip_contract_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME two_chip_contract_test COMMAND two_chip_contract_test)

# tools/check_layers.py: the layer checker, rules A/B/C on fixture components and every ratchet state (#218)
add_test(NAME check_layers_test COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/check_layers_test.py)
add_test(NAME reserved_ids_test COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/reserved_ids_test.py)
# tools/check_rules.py: critical-section PSRAM, log level, task stack class, LVGL-in-task checks and their ratchet (#287)
add_test(NAME check_rules_test COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/tests/check_rules_test.py)

# app_main's start order, the two-chip start rules (#210, #234)
add_executable(boot_order_test
    ${CMAKE_CURRENT_SOURCE_DIR}/boot_order_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/boot_order.c
)
target_include_directories(boot_order_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)
target_compile_options(boot_order_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME boot_order_test COMMAND boot_order_test)

# Which way a power off goes (#281, #292): the Power app, main's headless path, or nothing twice
add_executable(power_route_test
    ${CMAKE_CURRENT_SOURCE_DIR}/power_route_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main/power_route.c
)
target_include_directories(power_route_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)
target_compile_options(power_route_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME power_route_test COMMAND power_route_test)

# The OS name on the boot screen and in About (#236)
add_executable(os_name_test
    ${CMAKE_CURRENT_SOURCE_DIR}/os_name_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/os_name.c
)
target_include_directories(os_name_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api)
target_compile_options(os_name_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME os_name_test COMMAND os_name_test)

# The gate forwards lichen_ui.h / lichen_keys.h to the shell's table, with a defined answer before it is set (#221)
add_executable(ui_ops_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_ops_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api/lichen_ui_ops.c
)
target_include_directories(ui_ops_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs_lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen_api
)
target_compile_options(ui_ops_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ui_ops_test COMMAND ui_ops_test)
