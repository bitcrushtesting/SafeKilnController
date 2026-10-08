# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host test build (architecture section 14.2).  No ESP-IDF involved: the core is
# plain C++20, so it builds in seconds and runs under a debugger, ASan/UBSan (TR-20)
# and gcov (TR-19).
#
#   cmake -B build-host -S firmware/controller/test/host -DENABLE_COVERAGE=ON -DENABLE_ASAN=ON
#   cmake --build build-host && ctest --test-dir build-host

cmake_minimum_required(VERSION 3.16)
project(kiln_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_COVERAGE "Build with gcov instrumentation (TR-19)" OFF)
option(ENABLE_ASAN     "Build with AddressSanitizer + UBSan (TR-20)" OFF)

# NFR-25: warnings are errors here, where the logic lives.
add_compile_options(-Wall -Wextra -Werror -Wshadow)

if(ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

set(KILN_FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${KILN_FW_ROOT}/components/kiln_ports ${CMAKE_BINARY_DIR}/kiln_ports)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_core  ${CMAKE_BINARY_DIR}/kiln_core)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_sim   ${CMAKE_BINARY_DIR}/kiln_sim)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_app   ${CMAKE_BINARY_DIR}/kiln_app)
add_subdirectory(${KILN_FW_ROOT}/host/kiln_hal_host     ${CMAKE_BINARY_DIR}/kiln_hal_host)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_web   ${CMAKE_BINARY_DIR}/kiln_web)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_hmi   ${CMAKE_BINARY_DIR}/kiln_hmi)

add_library(kiln_check STATIC support/kiln_check.cpp)
target_include_directories(kiln_check PUBLIC support)

enable_testing()

# One executable per suite; the name is also the ctest name.
function(kiln_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kiln_web kiln_hmi kiln_app kiln_core kiln_sim kiln_hal_host kiln_check)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kiln_add_test(test_pid         unit/test_pid.cpp)
kiln_add_test(test_window      unit/test_window.cpp)
kiln_add_test(test_suplink     unit/test_suplink.cpp)
kiln_add_test(test_tempfilt    unit/test_tempfilt.cpp)
kiln_add_test(test_setpoint    unit/test_setpoint.cpp)
kiln_add_test(test_profile     unit/test_profile.cpp)
kiln_add_test(test_autotune    unit/test_autotune.cpp)
kiln_add_test(test_current     unit/test_current.cpp)
kiln_add_test(test_configmodel unit/test_configmodel.cpp)
kiln_add_test(test_runstate    unit/test_runstate.cpp)
kiln_add_test(test_logrec      unit/test_logrec.cpp)
kiln_add_test(test_logring     unit/test_logring.cpp)
kiln_add_test(test_fileslots   unit/test_fileslots.cpp)
kiln_add_test(test_stores      unit/test_stores.cpp)
kiln_add_test(test_faults      unit/test_faults.cpp)
kiln_add_test(test_json        unit/test_json.cpp)
kiln_add_test(test_safety_thermal unit/test_safety_thermal.cpp)
kiln_add_test(test_safety_current unit/test_safety_current.cpp)
kiln_add_test(test_safety_door    unit/test_safety_door.cpp)
kiln_add_test(test_hmi            unit/test_hmi.cpp)

kiln_add_test(test_integration integration/test_integration.cpp)
kiln_add_test(test_persistence integration/test_persistence.cpp)
kiln_add_test(test_stores_on_flash integration/test_stores_on_flash.cpp)
kiln_add_test(test_api         integration/test_api.cpp)
kiln_add_test(test_fuzz_logrec fuzz/test_fuzz_logrec.cpp)
