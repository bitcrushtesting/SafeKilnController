# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The API and JSON layer is platform-free and dual-target, so the host test
# suite and the host harness drive exactly the code the device runs.  The
# esp_http_server transport is the only target-only part of it.

set(KILN_WEB_SRCS
    src/json.cpp
    src/api.cpp
    src/api_log.cpp
)

if(ESP_PLATFORM)
    # httpd.cpp is the socket half and only exists on the target; everything
    # it serves is decided in api.cpp, which the host suite covers.
    idf_component_register(
        SRCS ${KILN_WEB_SRCS} "src/httpd.cpp"
        INCLUDE_DIRS include
        REQUIRES kiln_app kiln_core kiln_ports esp_http_server
    )
else()
    add_library(kiln_web STATIC ${KILN_WEB_SRCS})
    target_include_directories(kiln_web PUBLIC include)
    target_link_libraries(kiln_web PUBLIC kiln_app kiln_core kiln_ports)
    set_target_properties(kiln_web PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()
