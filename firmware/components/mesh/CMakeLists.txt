# MeshCore's companion (meshcore/ submodule: src/ and examples/companion_radio, built unchanged) with rweather's Crypto
# and our Arduino shim, as host_tests/CMakeLists.txt builds it (meshcore_companion_host, #65); the badge target is
# companion/target.h. Pure parts (mesh_store, mesh_format, mesh_tile, lora_airtime, radio_presets,
# channel_input) are host-tested.
# meshcore_app.c makes it a Lichen app (#67), meshcore_settings.c its Settings page: mesh depends on lichen_api, never
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
idf_component_register(SRCS "mesh_task.cpp" "mesh_link.cpp" "mesh_frame.c" "mesh_outbox.c" "lora_radio.cpp" "mesh_store.c" "lora_airtime.c"
                            "mesh_hist.c" "mesh_hist_crypto.cpp" "mesh_journal.c" "mesh_format.c" "mesh_contact_fmt.c" "mesh_tile.c"
                            "meshcore_hdr.c" "meshcore_app.c" "meshcore_chat.c" "meshcore_contact.c" "mesh_chat.c" "meshcore_settings.c" "radio_presets.c" "radio_filter.c" "radio_confirm.c" "channel_input.c" "mc_first.c"
                            "../../../shared/arduino_shim/arduino_shim.cpp"
                            "../../../shared/arduino_shim/fs_shim.cpp" ${THIRD_PARTY_SRCS} ${COMPANION_SRCS}
                       INCLUDE_DIRS "."
                       PRIV_INCLUDE_DIRS "companion" ${MC}/src ${MC}/examples/companion_radio ${MC}/lib/ed25519 ${CR}
                                         "../../../shared/arduino_shim" "../../../third_party/base64/src"
                       PRIV_REQUIRES lichen_clock lora fw_console nvs_flash esp_timer lichen_random freertos
                                     esp_system heap lichen_api esp_lvgl_port sdcard)
# MeshCore's sizes on the badge: its tables live in PSRAM (mesh_task.cpp), so far beyond MeshCore's defaults
target_compile_definitions(${COMPONENT_LIB} PRIVATE MAX_CONTACTS=1000 MAX_GROUP_CHANNELS=40 OFFLINE_QUEUE_SIZE=256
                           LORA_FREQ=869.618 LORA_BW=62.5 LORA_SF=7 LORA_CR=5 LORA_TX_POWER=22 MAX_LORA_TX_POWER=22)
set_source_files_properties(${COMPANION_SRCS} "mesh_task.cpp" "mesh_link.cpp" PROPERTIES COMPILE_DEFINITIONS ESP32)
# The app's factory reset must not erase NVS (Wi-Fi credentials): see mesh_nvs_flash_erase() in mesh_task.cpp
set_property(SOURCE ${MC}/examples/companion_radio/DataStore.cpp APPEND PROPERTY COMPILE_DEFINITIONS
             nvs_flash_erase=mesh_nvs_flash_erase)
# Third-party code is built as is
set_source_files_properties(${THIRD_PARTY_SRCS} ${COMPANION_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
# MeshCore's headers initialise members out of declaration order and memset() non-trivial structs
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wno-reorder;-Wno-class-memaccess>")
