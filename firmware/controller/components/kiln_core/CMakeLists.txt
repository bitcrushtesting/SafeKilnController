# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dual-target build (SWA-14, architecture section 14.2): the same sources compile
# as an ESP-IDF component and as a plain CMake library for the host tests.  The
# core may not include a single IDF header -- tools/layercheck enforces that.

set(KILN_CORE_SRCS
    src/alarmptn.cpp
    src/autotune.cpp
    src/configmodel.cpp
    src/current.cpp
    src/err.cpp
    src/fileslots.cpp
    src/faults.cpp
    src/logrec.cpp
    src/logring.cpp
    src/pid.cpp
    src/profile.cpp
    src/runstate.cpp
    src/safety.cpp
    src/setpoint.cpp
    src/suplink.cpp
    src/tempfilt.cpp
    src/window.cpp
    # The supervisor link's wire format, owned by firmware/supervisor/protocol and
    # compiled into both firmwares (SWA-22).  Declarative plus a CRC, which is
    # the only behaviour the two ends share; a CRC that disagreed would show up
    # as rejected frames, which both sides already treat as silence.
    ${CMAKE_CURRENT_LIST_DIR}/../../../supervisor/protocol/src/sup_proto.cpp
)
set(KILN_SUP_PROTO_INC ${CMAKE_CURRENT_LIST_DIR}/../../../supervisor/protocol/include)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KILN_CORE_SRCS}
        INCLUDE_DIRS include ${KILN_SUP_PROTO_INC}
        REQUIRES kiln_ports
    )
else()
    add_library(kiln_core STATIC ${KILN_CORE_SRCS})
    target_include_directories(kiln_core PUBLIC include ${KILN_SUP_PROTO_INC})
    target_link_libraries(kiln_core PUBLIC kiln_ports)
    # The core is plain C++20 with no platform dependency; libm for the autotune.
    set_target_properties(kiln_core PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    find_library(KILN_LIBM m)
    if(KILN_LIBM)
        target_link_libraries(kiln_core PUBLIC ${KILN_LIBM})
    endif()
endif()
