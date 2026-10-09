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

    # --- the browser interface, gzipped into the image (SWA-11, SWR-WEB-02)
    #
    # Gzipped at build time rather than committed compressed: the source of
    # truth stays the readable file in web/, and nobody has to remember to
    # recompress after an edit.  `gzip -9 -n` for the -n: without it the
    # header carries the mtime and the image stops being reproducible from
    # the same sources, which tools/sbom.py and the release both assume.
    #
    # Served with Content-Encoding: gzip rather than decompressed on the
    # device.  Every browser that can run the interface can inflate it, and
    # doing it here costs 38 kB of flash and no RAM at all, where inflating
    # on demand would cost a window buffer per request on the processor that
    # is also running a kiln.
    set(KILN_WEB_ASSETS index.html app.css app.js chart.js)
    set(KILN_WEB_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../web")
    set(KILN_WEB_GZ_DIR  "${CMAKE_CURRENT_BINARY_DIR}/assets")
    file(MAKE_DIRECTORY "${KILN_WEB_GZ_DIR}")

    foreach(asset ${KILN_WEB_ASSETS})
        set(src "${KILN_WEB_SRC_DIR}/${asset}")
        set(gz  "${KILN_WEB_GZ_DIR}/${asset}.gz")
        if(NOT EXISTS "${src}")
            message(FATAL_ERROR
                "web asset ${src} is missing: the interface would build "
                "without it and 404 at run time, which is worse than failing "
                "here")
        endif()
        add_custom_command(
            OUTPUT  "${gz}"
            COMMAND gzip -9 -n -c "${src}" > "${gz}"
            DEPENDS "${src}"
            COMMENT "gzip ${asset}"
            VERBATIM
        )
        list(APPEND KILN_WEB_GZ "${gz}")
        target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY)
    endforeach()

    # The custom commands are attached to a target of their own so they run
    # before the component is linked; target_add_binary_data alone does not
    # create that dependency for a generated file.
    add_custom_target(kiln_web_assets DEPENDS ${KILN_WEB_GZ})
    add_dependencies(${COMPONENT_LIB} kiln_web_assets)
else()
    add_library(kiln_web STATIC ${KILN_WEB_SRCS})
    target_include_directories(kiln_web PUBLIC include)
    target_link_libraries(kiln_web PUBLIC kiln_app kiln_core kiln_ports)
    set_target_properties(kiln_web PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
endif()
