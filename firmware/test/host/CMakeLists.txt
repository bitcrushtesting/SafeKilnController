# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host test build (architecture section 14.2).  No ESP-IDF involved: the core is
# plain C, so it builds in seconds and runs under a debugger, ASan/UBSan (TR-20)
# and gcov (TR-19).
#
#   cmake -B build-host -S firmware/test/host -DENABLE_COVERAGE=ON -DENABLE_ASAN=ON
#   cmake --build build-host && ctest --test-dir build-host

cmake_minimum_required(VERSION 3.16)
project(kiln_host_tests C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

add_library(kiln_check STATIC support/kiln_check.c)
target_include_directories(kiln_check PUBLIC support)

enable_testing()

# One executable per suite; the name is also the ctest name.
function(kiln_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE kiln_app kiln_core kiln_sim kiln_check)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kiln_add_test(test_pid         unit/test_pid.c)
kiln_add_test(test_window      unit/test_window.c)
kiln_add_test(test_tempfilt    unit/test_tempfilt.c)
kiln_add_test(test_setpoint    unit/test_setpoint.c)
kiln_add_test(test_profile     unit/test_profile.c)
kiln_add_test(test_autotune    unit/test_autotune.c)
kiln_add_test(test_current     unit/test_current.c)
kiln_add_test(test_configmodel unit/test_configmodel.c)
kiln_add_test(test_runstate    unit/test_runstate.c)
kiln_add_test(test_logrec      unit/test_logrec.c)
kiln_add_test(test_faults      unit/test_faults.c)
kiln_add_test(test_safety_thermal unit/test_safety_thermal.c)
kiln_add_test(test_safety_current unit/test_safety_current.c)

kiln_add_test(test_integration integration/test_integration.c)
kiln_add_test(test_fuzz_logrec fuzz/test_fuzz_logrec.c)
