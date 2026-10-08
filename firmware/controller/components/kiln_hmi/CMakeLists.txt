# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dual-target (SWA-14): the same sources build as an ESP-IDF component and as a
# plain CMake library for the host tests.  kiln_hmi holds no platform header
# for the same reason kiln_core does not -- its screens are logic, and SWR-TST-01
# wants logic where a test can reach it.

set(KILN_HMI_SRCS
    src/draw.cpp
    src/hmi.cpp
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KILN_HMI_SRCS}
        INCLUDE_DIRS include
        REQUIRES kiln_ports kiln_core
    )
else()
    add_library(kiln_hmi STATIC ${KILN_HMI_SRCS})
    target_include_directories(kiln_hmi PUBLIC include)
    target_link_libraries(kiln_hmi PUBLIC kiln_ports kiln_core)
    set_target_properties(kiln_hmi PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()
