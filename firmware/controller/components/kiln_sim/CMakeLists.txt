# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Plant simulator (SWR-TST-11).  Dual-target like the core: the host integration suite
# links it, and so does the target build when the firmware is configured with the
# simulated plant -- which is how it runs under QEMU.

set(KILN_SIM_SRCS src/sim.cpp)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KILN_SIM_SRCS}
        INCLUDE_DIRS include
        REQUIRES kiln_ports
    )
else()
    add_library(kiln_sim STATIC ${KILN_SIM_SRCS})
    target_include_directories(kiln_sim PUBLIC include)
    target_link_libraries(kiln_sim PUBLIC kiln_ports)
    set_target_properties(kiln_sim PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    find_library(KILN_SIM_LIBM m)
    if(KILN_SIM_LIBM)
        target_link_libraries(kiln_sim PUBLIC ${KILN_SIM_LIBM})
    endif()
endif()
