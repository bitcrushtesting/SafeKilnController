# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The composition root, and nothing else (architecture section 5.3): this is the
# only place that binds a port to a concrete adapter.

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS ""
    REQUIRES kiln_app kiln_core kiln_ports kiln_sim esp_timer driver
)
