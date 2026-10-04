# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Interface headers only -- no sources, by design (architecture section 4 rule 2).
# Dual-target (AD-14): an IDF component on the target, an INTERFACE library on
# the host, so the same include path serves both builds.

if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS include)
else()
    add_library(kiln_ports INTERFACE)
    target_include_directories(kiln_ports INTERFACE include)
endif()
