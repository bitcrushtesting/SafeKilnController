# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ESP-IDF project root (CON-01, ESP-IDF 5.x).  Target: esp32s3 (CON-02).
#
# The host test build is a separate CMake project under test/host and does not go
# through this file -- that is AD-14, and tools/layercheck keeps it honest.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kilncontrol)
