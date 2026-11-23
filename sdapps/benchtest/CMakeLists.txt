# Bench test (scaffolded by `tools/badge.sh new-app --sd`): an SD-card app, a standalone ELF. Everything shared is in
# ../sdapp.cmake; the code is in main/. firmware/build.sh build builds it into firmware/build/sdapps/benchtest/.
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../sdapp.cmake)
project(benchtest)
sdapp_elf(benchtest)
