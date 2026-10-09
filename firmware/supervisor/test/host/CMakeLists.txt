# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host tests for the supervisor.  Plain CMake, no ARM toolchain needed: the
# trip logic and the wire format are the whole safety function and both are
# platform-free by construction (SWA-22), so both are exercised here.
#
#   cmake -B build-sup -S firmware/supervisor/test/host [-DENABLE_ASAN=ON]
#   cmake --build build-sup && ctest --test-dir build-sup
cmake_minimum_required(VERSION 3.16)
project(kiln_supervisor_tests CXX)

# C++17 to match firmware/supervisor/CMakeLists.txt: the suites compile the same
# core sources, and a test build at a different standard tests a different
# translation of the code than the one that ships.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wshadow)

# SWR-TST-20, the same option name and the same sanitiser pair the controller's
# host build carries, so one flag means one thing across the repository.
#
# It earns its place here for a reason the controller's does not share.  The
# supervisor's arithmetic is all fixed point: a 19-bit field reassembled across
# three register bytes, a sign-extending shift, a magnitude taken in unsigned
# arithmetic, saturating millisecond timers.  Every one of those has an
# undefined-behaviour failure mode that is invisible at -O0 on x86 and would
# be a wrong temperature on the target: a signed shift overflowing, a negation
# of INT32_MIN, a multiply leaving int32_t.  UBSan is the tool that catches
# exactly that class, and the trip logic is the last place to find it by
# reading.
#
# Note what is deliberately NOT enabled: unsigned-integer-overflow.  add_ms and
# the disagreement magnitude both RELY on unsigned wrap being defined, which it
# is, so that check would report the two places most carefully written.
# -fno-sanitize-recover=undefined is not optional and is the reason this block
# is not a two-line copy of the controller's.  UBSan's DEFAULT is to print a
# runtime error and carry on, leaving the exit code at zero: a test that
# triggers real undefined behaviour reports it on stderr and ctest calls it
# PASSED.  A gate that prints the finding and goes green is worse than no gate,
# because it looks like one.  This makes the first finding abort.
#
# Checked rather than assumed: reintroducing the signed `hi - lo` that
# `disagreeing` deliberately avoids produces "signed integer overflow" from
# test_trip and a non-zero exit, and removing this option makes the same run
# pass.  ASan already aborts by default; it is UBSan that needed saying.
# `enum` is excluded to match the controller's block, where three negative
# tests cast out-of-range values to an enum on purpose; see that file.  The
# supervisor's sup_decode writes a wire byte into a sup_trip_reason_t the same
# way, so a test for an unrecognised reason would trip it here too.  One flag
# meaning one thing in both halves is worth more than enabling a check on the
# half that happens not to exercise it yet.
option(ENABLE_ASAN "Build with AddressSanitizer + UBSan (SWR-TST-20)" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize=enum
                        -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined -fno-sanitize=enum
                     -fno-sanitize-recover=undefined)
endif()

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
