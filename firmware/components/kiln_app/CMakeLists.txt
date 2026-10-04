# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later

set(KILN_APP_SRCS
    src/app.c
    src/program_store.c
    src/run_index.c
    src/settings.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KILN_APP_SRCS}
        INCLUDE_DIRS include
        REQUIRES kiln_core kiln_ports
    )
else()
    add_library(kiln_app STATIC ${KILN_APP_SRCS})
    target_include_directories(kiln_app PUBLIC include)
    target_link_libraries(kiln_app PUBLIC kiln_core kiln_ports)
    set_target_properties(kiln_app PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
endif()
