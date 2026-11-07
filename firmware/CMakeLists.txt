# Lean badge firmware (#61): one plain ESP-IDF application on the P4, no BadgeVMS.
# Only stock esp-idf components plus our own under components/.
cmake_minimum_required(VERSION 3.16)

set(ENV{IDF_TARGET} esp32p4)

set(EXTRA_COMPONENT_DIRS components)
# Our own version name (#63): without it ESP-IDF takes `git describe --tags`, whose nearest tag is
# BadgeVMS's. The app description keeps 31 characters, e.g. fw-v0.1.0-3-gabc1234-dirty fits.
execute_process(
    COMMAND git describe --tags --match "fw-v*" --always --dirty
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE fw_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE fw_describe_result)
if(fw_describe_result EQUAL 0)
    set(PROJECT_VER "fw-${fw_describe}")
else()
    set(PROJECT_VER "fw-unknown")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(badge_fw)

# Stack frame sizes of our own components (#202): -fstack-usage writes a .su file next to each object and does not
# change the generated code; tools/check_standard.py reads them. Stock IDF components are left alone.
file(GLOB lichen_component_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/components
     ${CMAKE_CURRENT_SOURCE_DIR}/components/*)
foreach(comp main ${lichen_component_dirs})
    if(NOT comp STREQUAL "main" AND NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/components/${comp})
        continue()
    endif()
    idf_component_get_property(comp_lib ${comp} COMPONENT_LIB)
    if(comp_lib AND TARGET ${comp_lib})
        target_compile_options(${comp_lib} PRIVATE -fstack-usage)
    endif()
endforeach()
