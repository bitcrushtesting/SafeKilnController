# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host-only development harness.  Outside firmware/components/ for the same
# reason kiln_hal_host is: ESP-IDF would otherwise try to build it into the image.

cmake_minimum_required(VERSION 3.16)
project(kiln_webhost CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(KILN_FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_subdirectory(${KILN_FW_ROOT}/components/kiln_ports ${CMAKE_BINARY_DIR}/kiln_ports)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_core  ${CMAKE_BINARY_DIR}/kiln_core)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_sim   ${CMAKE_BINARY_DIR}/kiln_sim)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_app   ${CMAKE_BINARY_DIR}/kiln_app)
add_subdirectory(${KILN_FW_ROOT}/components/kiln_web   ${CMAKE_BINARY_DIR}/kiln_web)
add_subdirectory(${KILN_FW_ROOT}/host/kiln_hal_host    ${CMAKE_BINARY_DIR}/kiln_hal_host)

add_executable(webhost webhost.cpp)
target_link_libraries(webhost PRIVATE kiln_web kiln_app kiln_core kiln_sim kiln_hal_host)
# strcasestr is a GNU/BSD extension; _GNU_SOURCE gets it on glibc and it is in
# string.h on macOS.
target_compile_definitions(webhost PRIVATE _GNU_SOURCE)
