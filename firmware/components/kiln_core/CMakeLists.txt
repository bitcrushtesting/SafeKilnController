# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dual-target build (AD-14, architecture section 14.2): the same sources compile
# as an ESP-IDF component and as a plain CMake library for the host tests.  The
# core may not include a single IDF header -- tools/layercheck enforces that.

set(KILN_CORE_SRCS
    src/autotune.c
    src/configmodel.c
    src/current.c
    src/err.c
    src/faults.c
    src/logrec.c
    src/pid.c
    src/profile.c
    src/runstate.c
    src/safety.c
    src/setpoint.c
    src/tempfilt.c
    src/window.c
)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KILN_CORE_SRCS}
        INCLUDE_DIRS include
        REQUIRES kiln_ports
    )
else()
    add_library(kiln_core STATIC ${KILN_CORE_SRCS})
    target_include_directories(kiln_core PUBLIC include)
    target_link_libraries(kiln_core PUBLIC kiln_ports)
    # The core is plain C99 with no platform dependency; libm for the autotune.
    set_target_properties(kiln_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
    find_library(KILN_LIBM m)
    if(KILN_LIBM)
        target_link_libraries(kiln_core PUBLIC ${KILN_LIBM})
    endif()
endif()
