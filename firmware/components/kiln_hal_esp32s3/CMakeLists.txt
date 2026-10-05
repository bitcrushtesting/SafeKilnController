# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target adapters.  Target-only by nature, and unlike kiln_hal_host it belongs
# under components/ because it *is* part of the image.

if(NOT ESP_PLATFORM)
    message(FATAL_ERROR "kiln_hal_esp32s3 builds only for the target")
endif()

idf_component_register(
    SRCS
        "src/hal_flash.cpp"
        "src/hal_kvstore.cpp"
        "src/hal_clock.cpp"
        "src/hal_system.cpp"
    INCLUDE_DIRS "include"
    REQUIRES kiln_ports esp_timer
    PRIV_REQUIRES spi_flash nvs_flash esp_app_format esp_system esp_hal_wdt
)
