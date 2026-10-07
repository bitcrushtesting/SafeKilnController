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
        "src/hal_tc.cpp"
        "src/hal_heat.cpp"
        "src/hal_current.cpp"
        "src/hal_io.cpp"
        "src/hal_counters.cpp"
        "src/hal_display.cpp"
        "src/hal_input.cpp"
        "src/hal_net.cpp"
        "src/hal_suplink.cpp"
    INCLUDE_DIRS "include"
    REQUIRES kiln_ports kiln_core esp_timer
    # ESP-IDF 6.0 split the monolithic `driver` component per peripheral, so ask
    # for the three that are used rather than dragging the rest in.
    PRIV_REQUIRES spi_flash nvs_flash esp_app_format esp_system esp_hal_wdt
                  esp_driver_spi esp_driver_gpio esp_adc
                  esp_driver_i2c esp_driver_pcnt esp_driver_uart
                  esp_wifi esp_netif esp_event lwip
)
