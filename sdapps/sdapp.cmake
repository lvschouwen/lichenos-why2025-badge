# The shared CMake of every SD-card app (#522, #516): an app's CMakeLists.txt is
#     cmake_minimum_required(VERSION 3.16)
#     include(${CMAKE_CURRENT_LIST_DIR}/../sdapp.cmake)
#     project(<id>)
#     sdapp_elf(<id>)
# project() stays in the app's own file: CMake looks for the call there (textually) and otherwise configures with the
# host compiler.
# and its code lives in main/ (idf_component_register(SRCS "app.c")). The app is a standalone ELF, not a firmware: only
# `main` is built, linked with the ELF loader's helper (project_elf: -fPIC -shared -nostdlib -nostartfiles
# -fvisibility=hidden, -e app_main, -Dmain=app_main, gc-sections, stripped; P4 march from the IDF target). The OS resolves
# every unresolved symbol against its export table when it loads the app. firmware/build.sh build_sdapps runs this after
# the P4 build, which fetches the loader component and writes the firmware's config.
#
# Headers: an app includes lichen_api's public headers (tools/check_app_boundary.sh checks it). Those pull in LVGL, so
# the app gets the LVGL include dir and the firmware's CONFIG_LV_* values (a forced include generated from the
# firmware's sdkconfig.h): the LVGL structs then have the layout the firmware's LVGL has.
set(SDAPP_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(SDAPP_FW_SDKCONFIG "${SDAPP_ROOT}/firmware/build/config/sdkconfig.h")

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

macro(sdapp_elf app_id)
    include(${SDAPP_ROOT}/firmware/managed_components/espressif__elf_loader/elf_loader.cmake)
    project_elf(${app_id})
    if(NOT EXISTS "${SDAPP_FW_SDKCONFIG}")
        message(FATAL_ERROR "sdapp: ${SDAPP_FW_SDKCONFIG} is missing: build the firmware first (firmware/build.sh build)")
    endif()
    file(STRINGS "${SDAPP_FW_SDKCONFIG}" _sdapp_lv REGEX "^#define CONFIG_LV_")
    string(REPLACE ";" "\n" _sdapp_lv "${_sdapp_lv}")
    file(WRITE "${CMAKE_BINARY_DIR}/sdapp_lv_config.h" "#pragma once\n${_sdapp_lv}\n")
    # one list argument: idf_build_set_property takes a single value
    set(_sdapp_opts "-I${SDAPP_ROOT}/firmware/components/lichen_api"
                    "-I${SDAPP_ROOT}/firmware/managed_components/lvgl__lvgl"
                    "-DLV_CONF_INCLUDE_SIMPLE"
                    "-include${CMAKE_BINARY_DIR}/sdapp_lv_config.h")
    idf_build_set_property(COMPILE_OPTIONS "${_sdapp_opts}" APPEND)
endmacro()
