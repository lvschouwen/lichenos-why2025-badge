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
