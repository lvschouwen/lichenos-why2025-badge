# MeshCore's companion (meshcore/ submodule: src/ and examples/companion_radio, built unchanged) with rweather's Crypto
# and our Arduino shim, as host_tests/CMakeLists.txt builds it (meshcore_companion_host, #65); the badge target is
# companion/target.h. Pure parts (mesh_store, mesh_format, mesh_tile, lora_airtime, radio_presets,
# channel_input, mesh_grid, advert_limit, mesh_applied) are host-tested.
# meshcore_app.c makes it a Lichen app (#67), meshcore_settings.c its Settings page; the app opens on its own grid of
# tiles (#88: meshcore_grid.c on the pure mesh_grid.c, the lists in meshcore_chats.c and meshcore_contacts.c): mesh depends on lichen_api, never
# the other way round, and never on lichen_shell (#84).
set(MC ../../../meshcore)
set(CR ../../../third_party/arduinolibs/libraries/Crypto)
file(GLOB ED25519_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/${MC}/lib/ed25519/*.c)
set(THIRD_PARTY_SRCS
    ${MC}/src/Dispatcher.cpp ${MC}/src/Mesh.cpp ${MC}/src/Packet.cpp ${MC}/src/Identity.cpp ${MC}/src/Utils.cpp
    ${MC}/src/helpers/BaseChatMesh.cpp ${MC}/src/helpers/AdvertDataHelpers.cpp ${MC}/src/helpers/TxtDataHelpers.cpp
    ${MC}/src/helpers/StaticPoolPacketManager.cpp ${MC}/src/helpers/ConfigSerializer.cpp
    ${MC}/src/helpers/DynamicConfigSerializer.cpp ${MC}/src/helpers/CommonRadioPrefs.cpp
    ${CR}/AES128.cpp ${CR}/AESCommon.cpp ${CR}/SHA256.cpp ${CR}/SHA512.cpp ${CR}/Ed25519.cpp ${CR}/Curve25519.cpp
    ${CR}/BigNumberUtil.cpp ${CR}/Crypto.cpp ${CR}/Hash.cpp ${CR}/BlockCipher.cpp
    ${ED25519_SRCS})
# ESP32 picks the fs::FS file system in IdentityStore.h (and SPIFFS in DataStore): only the files that include it get
# it, because elsewhere it would switch rweather Crypto to the ESP32's hardware AES
set(COMPANION_SRCS
    ${MC}/src/helpers/IdentityStore.cpp ${MC}/src/helpers/RegionMap.cpp ${MC}/src/helpers/TransportKeyStore.cpp
    ${MC}/examples/companion_radio/MyMesh.cpp ${MC}/examples/companion_radio/DataStore.cpp)
# The app's allowlist (#84 T9, docs/app-api.md): Lichen's app API and services, the radio, the console, and plain
# libraries. The build is the check: a header from any other component does not resolve. The card goes through
# lichen_fs.h (the SD batch calls, #98), never the sdcard component. Results of the settings jobs reach the LVGL task
# through lichen_ui_call (#84 T8a), so esp_lvgl_port is not needed.
set(MESH_REQUIRES lichen_api lichen_clock lichen_random lora fw_console freertos esp_timer heap mbedtls)
idf_component_register(SRCS "mesh_task.cpp" "mesh_api_view.cpp" "mesh_api_settings.cpp" "mesh_api_channels.cpp" "mesh_api_send.cpp" "mesh_api_diag.cpp" "mesh_cmd.cpp" "mesh_cmd_settings.cpp" "mesh_cmd_send.cpp" "mesh_cmd_remote.cpp" "mesh_cmd_store.cpp"
                            "mesh_cmd_table.c" "mesh_link.cpp" "mesh_frame.c" "mesh_outbox.c" "mesh_trace.c" "mesh_remote.c" "mesh_telem.c" "meshcore_telem.c" "mesh_rset.c" "mesh_rset_run.c" "mesh_rhist.c" "mesh_rhist_run.c" "meshcore_rhist.c" "meshcore_remote.c" "meshcore_rset.c" "lora_radio.cpp" "mesh_store.c" "mesh_mem.c" "lora_airtime.c" "advert_limit.c" "mesh_blobq.c" "mesh_advert_cfg.c" "meshcore_app_record.c" "mesh_scope.c" "mesh_cscope.c" "mesh_onair.c" "mesh_onair_run.c"
                            "mesh_hist.c" "mesh_hist_boot.c" "mesh_hist_crypto.cpp" "mesh_hist_selftest.c" "mesh_ident.c" "mesh_journal.c" "mesh_journal_gen.c" "mesh_journal_scan.c" "mesh_format.c" "mesh_contact_fmt.c" "mesh_share.c" "mesh_tile.c" "mesh_notify.c" "mesh_notify_run.c" "meshcore_notify.c"
                            "meshcore_hdr.c" "mesh_health.c" "meshcore_health.c" "mesh_airlog.c" "meshcore_airlog.c" "mesh_discover.c" "meshcore_discover.c" "mesh_radar.c" "meshcore_radar.c" "mesh_signal.c" "meshcore_signal.c" "meshcore_app.c" "meshcore_run.c" "mesh_run.c" "meshcore_grid.c" "meshcore_grid_icons.c" "mesh_grid.c" "mesh_order.c" "meshcore_list.c" "meshcore_chats.c" "meshcore_contacts.c" "meshcore_chat.c" "meshcore_contact.c" "mesh_chat.c" "mesh_chat_cmd.c" "mesh_chat_target.c" "meshcore_settings.c" "meshcore_advert_settings.c" "radio_presets.c" "mesh_radio_line.c" "mesh_applied.c" "meshcore_applied.c" "radio_filter.c" "radio_confirm.c" "channel_input.c" "channel_key.c" "mc_first.c"
                            "../../../shared/arduino_shim/arduino_shim.cpp"
                            "../../../shared/arduino_shim/fs_shim.cpp" ${THIRD_PARTY_SRCS} ${COMPANION_SRCS}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "companion" ${MC}/src ${MC}/examples/companion_radio ${MC}/lib/ed25519 ${CR}
                                         "../../../shared/arduino_shim" "../../../third_party/base64/src"
                       PRIV_REQUIRES ${MESH_REQUIRES})
# MeshCore's sizes on the badge: its tables live in PSRAM (mesh_task.cpp), so far beyond MeshCore's defaults
target_compile_definitions(${COMPONENT_LIB} PRIVATE MAX_CONTACTS=1000 MAX_GROUP_CHANNELS=40 OFFLINE_QUEUE_SIZE=256
                           LORA_FREQ=869.618 LORA_BW=62.5 LORA_SF=7 LORA_CR=5 LORA_TX_POWER=22 MAX_LORA_TX_POWER=22)
# The files split out of mesh_task.cpp (#208) see BadgeMesh (MyMesh.h) as it does: with ESP32, or the class differs
set_source_files_properties(${COMPANION_SRCS} "mesh_task.cpp" "mesh_api_view.cpp" "mesh_api_settings.cpp" "mesh_api_channels.cpp" "mesh_api_send.cpp" "mesh_api_diag.cpp" "mesh_cmd.cpp" "mesh_cmd_settings.cpp" "mesh_cmd_send.cpp" "mesh_cmd_remote.cpp" "mesh_cmd_store.cpp"
                            "mesh_link.cpp" PROPERTIES COMPILE_DEFINITIONS ESP32)
# The app's factory reset must not erase NVS (Wi-Fi credentials): see mesh_nvs_flash_erase() in mesh_task.cpp
set_property(SOURCE ${MC}/examples/companion_radio/DataStore.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             nvs_flash_erase=mesh_nvs_flash_erase)
# The journal's crypto (#98): PSA Crypto on the badge (hardware AES/SHA), rweather's portable code only in the host tests
# Third-party code is built as is
set_source_files_properties(${THIRD_PARTY_SRCS} ${COMPANION_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
# MeshCore's headers initialise members out of declaration order and memset() non-trivial structs
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder;-Wno-class-memaccess>")
