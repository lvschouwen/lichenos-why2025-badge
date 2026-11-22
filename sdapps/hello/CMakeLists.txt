# sdapps/hello (#517): the smallest native SD-card app, a standalone ELF built with the ELF loader's project_elf()
# helper (the loader's flags: -fPIC -shared -nostdlib -nostartfiles -fvisibility=hidden, -e app_main, -Dmain=app_main,
# gc-sections, stripped). Built by firmware/build.sh into firmware/build/sdapps/hello/hello.app.elf; the badge loads it
# from /sd with "fw elf run" (developer mode). Only `main` is built: an app is not an IDF firmware.
cmake_minimum_required(VERSION 3.16)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hello)
# The loader component is not a dependency of the app, only its CMake helper is: use the copy the firmware build fetched
include(${CMAKE_CURRENT_LIST_DIR}/../../firmware/managed_components/espressif__elf_loader/elf_loader.cmake)
project_elf(hello)
