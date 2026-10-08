# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host tests for the supervisor.  Plain CMake, no ARM toolchain needed: the
# trip logic and the wire format are the whole safety function and both are
# platform-free by construction (SWA-22), so both are exercised here.
cmake_minimum_required(VERSION 3.16)
project(kiln_supervisor_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wshadow)

set(SUP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(FW_ROOT  ${CMAKE_CURRENT_LIST_DIR}/../../../controller)

add_library(sup_core STATIC
    ${SUP_ROOT}/core/src/trip.cpp
    ${SUP_ROOT}/core/src/max31856.cpp
    ${SUP_ROOT}/core/src/selfcheck.cpp
    ${SUP_ROOT}/protocol/src/sup_proto.cpp)
target_include_directories(sup_core PUBLIC
    ${SUP_ROOT}/core/include
    ${SUP_ROOT}/protocol/include)

# The ESP32 project's test harness: 120 lines of macros and no product code, so
# sharing it shares nothing SWA-22 cares about.  Reinventing it would be worse.
add_library(sup_check STATIC ${FW_ROOT}/test/host/support/kiln_check.cpp)
target_include_directories(sup_check PUBLIC ${FW_ROOT}/test/host/support)

enable_testing()
foreach(t test_trip test_proto test_max31856 test_selfcheck)
    add_executable(${t} ${CMAKE_CURRENT_LIST_DIR}/${t}.cpp)
    target_link_libraries(${t} PRIVATE sup_core sup_check)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
