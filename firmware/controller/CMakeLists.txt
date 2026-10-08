# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ESP-IDF project root (UR-CON-01, ESP-IDF 5.x).  Target: esp32s3 (UR-CON-02).
#
# The host test build is a separate CMake project under test/host and does not go
# through this file -- that is SWA-14, and tools/layercheck keeps it honest.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# C++20, and stated rather than inherited.  The firmware is C++ (SWA-20) and the
# 152 designated initialisers it carries are a C99 feature that C++ only
# regained in C++20 -- they are how the safety defaults are written, naming
# every field, so losing them would be a readability regression in exactly the
# code that can least afford one.  The host build sets the same standard in
# each component's non-ESP_PLATFORM branch; this is the target half, applied to
# every component rather than per component so the two cannot drift.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++20" APPEND)   # defined by project.cmake, so it must follow it

# --- the version, from the git tag (SWR-UPD-06) ---------------------------
#
# PROJECT_VER lands in the app descriptor, which is what `fw_info` reports, what
# the UI shows and what an OTA image is identified by, so it has to be derived
# rather than typed: a hand-maintained constant disagrees with the tag the moment
# anyone forgets it.
#
# The release tags are `v<major>.<minor>.<patch>` (see .github/workflows/release.yml)
# and the result is semantic versioning:
#
#   on the tag, clean    1.4.0
#   on the tag, dirty    1.4.0+dirty
#   12 commits past it   1.4.0+12.a1b2c3d
#   ... and dirty        1.4.0+12.a1b2c3d.dirty
#   no tag yet           0.0.0+a1b2c3d
#   not a git checkout   0.0.0
#
# Commit count and hash go in semver *build metadata* rather than a prerelease
# tag on purpose. `1.4.0-12` would sort BEFORE 1.4.0 under semver precedence,
# which is backwards: these builds come after it. Build metadata is ignored for
# precedence, which is the honest answer for "1.4.0 plus 12 commits".
#
# `--match` is not optional. This repository carries a non-release tag
# (`pre-force-push/...`), and a bare `git describe --tags` would happily name a
# firmware image after a branch backup.
set(KILN_TAG_GLOB "v[0-9]*.[0-9]*.[0-9]*")

function(kiln_git OUTVAR)
    execute_process(COMMAND ${GIT_EXECUTABLE} ${ARGN}
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE out
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        set(out "")
    endif()
    set(${OUTVAR} "${out}" PARENT_SCOPE)
endfunction()

find_package(Git QUIET)

set(KILN_SEMVER "0.0.0")
set(KILN_GIT_REV "unknown")

if(GIT_FOUND)
    kiln_git(KILN_HASH rev-parse --short=7 HEAD)
    kiln_git(KILN_DIRTY status --porcelain --untracked-files=no)
    kiln_git(KILN_LAST_TAG describe --tags --abbrev=0 --match "${KILN_TAG_GLOB}")

    if(KILN_DIRTY STREQUAL "")
        set(KILN_DIRTY_SUFFIX "")
    else()
        set(KILN_DIRTY_SUFFIX ".dirty")
    endif()

    if(NOT KILN_LAST_TAG STREQUAL "")
        string(REGEX REPLACE "^v" "" KILN_BASE "${KILN_LAST_TAG}")
        kiln_git(KILN_AHEAD rev-list "${KILN_LAST_TAG}..HEAD" --count)
        if(KILN_AHEAD STREQUAL "" OR KILN_AHEAD STREQUAL "0")
            # On the tag: the version is the release, and only a dirty tree
            # qualifies it.
            if(KILN_DIRTY_SUFFIX STREQUAL "")
                set(KILN_SEMVER "${KILN_BASE}")
            else()
                set(KILN_SEMVER "${KILN_BASE}+dirty")
            endif()
        else()
            set(KILN_SEMVER "${KILN_BASE}+${KILN_AHEAD}.${KILN_HASH}${KILN_DIRTY_SUFFIX}")
        endif()
    elseif(NOT KILN_HASH STREQUAL "")
        set(KILN_SEMVER "0.0.0+${KILN_HASH}${KILN_DIRTY_SUFFIX}")
    endif()

    if(NOT KILN_HASH STREQUAL "")
        set(KILN_GIT_REV "${KILN_HASH}${KILN_DIRTY_SUFFIX}")
    endif()
endif()

# PROJECT_VER is read by project() below, so it must be set before it.
set(PROJECT_VER "${KILN_SEMVER}")
message(STATUS "Safe Kiln Controller version ${PROJECT_VER} (rev ${KILN_GIT_REV})")

project(safekiln)

# The app descriptor carries the version; the bare revision is a separate field
# of kiln_fw_info_t, so it is passed in rather than re-derived from the version
# string at runtime.
idf_build_set_property(COMPILE_DEFINITIONS "KILN_GIT_REV=\"${KILN_GIT_REV}\"" APPEND)
