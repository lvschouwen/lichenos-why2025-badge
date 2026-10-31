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

add_executable(console_cmd_test
    ${CMAKE_CURRENT_SOURCE_DIR}/console_cmd_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console/console_cmd.c
)

target_compile_options(console_cmd_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_include_directories(console_cmd_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
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

add_executable(meshcore_hdr_test
    ${CMAKE_CURRENT_SOURCE_DIR}/meshcore_hdr_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora/meshcore_hdr.c
)

target_include_directories(meshcore_hdr_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lora)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota
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

# MeshCore's protocol code with rweather's Crypto and our Arduino shim, as the lean firmware builds it (#61)
set(MESHCORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../meshcore)
set(CRYPTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/arduinolibs/libraries/Crypto)
file(GLOB ED25519_SOURCES ${MESHCORE_DIR}/lib/ed25519/*.c)
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

add_executable(radio_presets_test
    ${CMAKE_CURRENT_SOURCE_DIR}/radio_presets_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_presets.c
)
target_include_directories(radio_presets_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(radio_presets_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(radio_presets_test PRIVATE m)
add_test(NAME radio_presets_test COMMAND radio_presets_test)

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

add_executable(mesh_tile_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mesh_tile_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_tile.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_store.c
)
target_include_directories(mesh_tile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mesh_tile_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mesh_tile_test COMMAND mesh_tile_test)

add_executable(crash_log_test
    ${CMAKE_CURRENT_SOURCE_DIR}/crash_log_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/crash/crash_log.c
)
target_include_directories(crash_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/crash)
target_compile_options(crash_log_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME crash_log_test COMMAND crash_log_test)

add_executable(boot_steps_test
    ${CMAKE_CURRENT_SOURCE_DIR}/boot_steps_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/boot_steps.c
)
target_include_directories(boot_steps_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(boot_steps_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME boot_steps_test COMMAND boot_steps_test)

add_executable(log_clean_test
    ${CMAKE_CURRENT_SOURCE_DIR}/log_clean_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/log_clean.c
)
target_include_directories(log_clean_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
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
target_include_directories(meshcore_companion_host SYSTEM PUBLIC
    ${MESHCORE_DIR}/src
    ${MESHCORE_DIR}/examples/companion_radio
    ${MESHCORE_DIR}/lib/ed25519
    ${CRYPTO_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/arduino_shim
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/base64/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/companion
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
target_compile_definitions(meshcore_companion_host PUBLIC MAX_CONTACTS=1000 MAX_GROUP_CHANNELS=40
                           OFFLINE_QUEUE_SIZE=256)
# ESP32 picks the fs::FS file system in IdentityStore.h (and SPIFFS in DataStore); only the files that include it get
# it, because elsewhere it would switch rweather Crypto to the ESP32's hardware AES
set(COMPANION_ESP32_SRCS ${MESHCORE_DIR}/src/helpers/IdentityStore.cpp ${MESHCORE_DIR}/src/helpers/RegionMap.cpp
    ${MESHCORE_DIR}/src/helpers/TransportKeyStore.cpp ${MESHCORE_DIR}/examples/companion_radio/MyMesh.cpp
    ${MESHCORE_DIR}/examples/companion_radio/DataStore.cpp ${CMAKE_CURRENT_SOURCE_DIR}/companion_test.cpp)
set_source_files_properties(${COMPANION_ESP32_SRCS} PROPERTIES COMPILE_DEFINITIONS ESP32)
target_compile_options(meshcore_companion_host PRIVATE -w)

add_executable(companion_test ${CMAKE_CURRENT_SOURCE_DIR}/companion_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/radio_presets.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mesh_format.c)
target_link_libraries(companion_test PRIVATE meshcore_companion_host)
target_include_directories(companion_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
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

add_executable(nav_test
    ${CMAKE_CURRENT_SOURCE_DIR}/nav_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/nav.c
)
target_include_directories(nav_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(nav_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME nav_test COMMAND nav_test)

add_executable(mc_first_test
    ${CMAKE_CURRENT_SOURCE_DIR}/mc_first_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh/mc_first.c
)
target_include_directories(mc_first_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/mesh)
target_compile_options(mc_first_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME mc_first_test COMMAND mc_first_test)

add_executable(ui_keys_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ui_keys_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/ui_keys.c
)
target_include_directories(ui_keys_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(ui_keys_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ui_keys_test COMMAND ui_keys_test)

add_executable(setup_state_test
    ${CMAKE_CURRENT_SOURCE_DIR}/setup_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/setup_state.c
)
target_include_directories(setup_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(setup_state_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME setup_state_test COMMAND setup_state_test)

add_executable(boot_text_test
    ${CMAKE_CURRENT_SOURCE_DIR}/boot_text_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/boot_text.c
)
target_include_directories(boot_text_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(boot_text_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME boot_text_test COMMAND boot_text_test)

add_executable(update_status_test
    ${CMAKE_CURRENT_SOURCE_DIR}/update_status_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/update_status.c
)
target_include_directories(update_status_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(update_status_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME update_status_test COMMAND update_status_test)

add_executable(update_mode_test
    ${CMAKE_CURRENT_SOURCE_DIR}/update_mode_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/update_mode.c
)
target_include_directories(update_mode_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(update_mode_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME update_mode_test COMMAND update_mode_test)

add_executable(home_layout_test
    ${CMAKE_CURRENT_SOURCE_DIR}/home_layout_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/home_layout.c
)
target_include_directories(home_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(home_layout_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME home_layout_test COMMAND home_layout_test)

add_executable(tz_table_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tz_table_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/tz_table.c
)
target_include_directories(tz_table_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(tz_table_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME tz_table_test COMMAND tz_table_test)

add_executable(wifi_list_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_list_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(wifi_list_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(wifi_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(wifi_list_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_list_test COMMAND wifi_list_test)

add_executable(link_state_test
    ${CMAKE_CURRENT_SOURCE_DIR}/link_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/link_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(link_state_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(link_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(link_state_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME link_state_test COMMAND link_state_test)

add_executable(wifi_cred_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_cred_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/wifi_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/nvs_stub.c
)
target_include_directories(wifi_cred_test BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(wifi_cred_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen
                           ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/fw_console)
target_compile_options(wifi_cred_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_cred_test COMMAND wifi_cred_test)

add_executable(wifi_join_test
    ${CMAKE_CURRENT_SOURCE_DIR}/wifi_join_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/wifi_join.c
)
target_include_directories(wifi_join_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(wifi_join_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME wifi_join_test COMMAND wifi_join_test)

add_executable(scroll_anchor_test
    ${CMAKE_CURRENT_SOURCE_DIR}/scroll_anchor_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/scroll_anchor.c
)
target_include_directories(scroll_anchor_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(scroll_anchor_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME scroll_anchor_test COMMAND scroll_anchor_test)

add_executable(shot_blend_test
    ${CMAKE_CURRENT_SOURCE_DIR}/shot_blend_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/shot_blend.c
)
target_include_directories(shot_blend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(shot_blend_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME shot_blend_test COMMAND shot_blend_test)

add_executable(log_ring_test
    ${CMAKE_CURRENT_SOURCE_DIR}/log_ring_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen/log_ring.c
)
target_include_directories(log_ring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/lichen)
target_compile_options(log_ring_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME log_ring_test COMMAND log_ring_test)

add_executable(ota_bmp_test
    ${CMAKE_CURRENT_SOURCE_DIR}/ota_bmp_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota/ota_bmp.c
)
target_include_directories(ota_bmp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ota)
target_compile_options(ota_bmp_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME ota_bmp_test COMMAND ota_bmp_test)

add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    DEPENDS assets_fmt_test font_cover_test c6_image_test ota_proto_test ota_pieces_test backlight_cmd_test backlight_level_test console_cmd_test lora_proto_test meshcore_hdr_test lora_link_test lora_irq_test console_cmd_fw_test c6_paths_test ota_path_test ota_valid_test kbd_map_test meshcore_decode_test mesh_store_test lora_airtime_test mesh_format_test radio_presets_test radio_confirm_test channel_input_test mesh_tile_test crash_log_test boot_steps_test log_clean_test fs_shim_test cayenne_lpp_test companion_test lora_tx_test nav_test home_layout_test tz_table_test wifi_list_test wifi_cred_test wifi_join_test setup_state_test mc_first_test ui_keys_test boot_text_test update_status_test update_mode_test link_state_test scroll_anchor_test shot_blend_test ota_bmp_test log_ring_test psram_pattern_test
    COMMENT "Running all host tests"
)
