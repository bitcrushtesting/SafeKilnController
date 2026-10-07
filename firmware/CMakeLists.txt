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

# C++20, and stated rather than inherited.  The firmware is C++ (AD-20) and the
# 152 designated initialisers it carries are a C99 feature that C++ only
# regained in C++20 -- they are how the safety defaults are written, naming
# every field, so losing them would be a readability regression in exactly the
# code that can least afford one.  The host build sets the same standard in
# each component's non-ESP_PLATFORM branch; this is the target half, applied to
# every component rather than per component so the two cannot drift.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++20" APPEND)   # defined by project.cmake, so it must follow it

project(safekiln)
