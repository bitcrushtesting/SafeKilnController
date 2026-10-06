# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The composition root, and nothing else (architecture section 5.3): this is the
# only place that binds a port to a concrete adapter.

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS ""
    REQUIRES kiln_app kiln_core kiln_ports kiln_sim kiln_hal_esp32s3 kiln_hmi esp_timer
    # ESP-IDF 6.0 split the monolithic `driver` component per peripheral, so ask
    # for the one that is actually used rather than dragging in I2C, TWAI and the
    # touch sensor to get a UART.
    PRIV_REQUIRES esp_driver_uart
)
