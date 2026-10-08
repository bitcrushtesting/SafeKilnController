/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor link's UART (AD-22).
 *
 * This file moves bytes off a UART and hands them to kiln_core/suplink, and
 * does nothing else.  Every decision about what those bytes mean -- frame
 * assembly, resynchronisation, a repeated sequence number, when a quiet link
 * becomes a fault -- lives in the core where a host test can drive it, which
 * is the same split AD-19 and AD-21 make for the log ring and the file store.
 *
 * Receive only.  No TX pin is assigned, so the peripheral has nothing to
 * transmit on: the simplex link of AD-22 is structural here and not merely a
 * convention this file happens to follow.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "kiln_suplink";

/* Two cycles of frames.  The supervisor sends 13 bytes at 10 Hz, so this is
 * generous; the point of a ring larger than one frame is that a late read
 * never loses one. */
constexpr int RX_RING_BYTES = 256;

}  // namespace

kiln_err_t kiln_hal_suplink_init()
{
    /* As elsewhere in this component: ESP-IDF's idiom is zero then assign, and
     * uart_config_t carries enums with no zero enumerator.  Every field this
     * code depends on is set below.
     * NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
    uart_config_t cfg = {};
    cfg.baud_rate = 115200;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    const uart_port_t port = (uart_port_t)KILN_HAL_SUP_UART;

    if (uart_driver_install(port, RX_RING_BYTES, 0, 0, nullptr, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed");
        return KILN_ERR_IO;
    }
    if (uart_param_config(port, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed");
        return KILN_ERR_IO;
    }
    /* RX only.  UART_PIN_NO_CHANGE for TX leaves no transmit pin bound to the
     * peripheral, so this firmware cannot send to the supervisor even if a
     * later change tried to: there is no pin to send on.  The supervisor has
     * no receiver wired either, which makes the simplex claim true at both
     * ends by construction rather than by agreement. */
    if (uart_set_pin(port, UART_PIN_NO_CHANGE, KILN_PIN_SUP_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed");
        return KILN_ERR_IO;
    }

    ESP_LOGI(TAG, "supervisor link on IO%d, UART%d, 115200 8N1, receive only",
             KILN_PIN_SUP_RX, KILN_HAL_SUP_UART);
    return KILN_OK;
}

size_t kiln_hal_suplink_read(uint8_t *out, size_t cap)
{
    if ((out == nullptr) || (cap == 0u)) {
        return 0;
    }
    /* Non-blocking: this is called from the acquisition cycle and must not
     * delay it (NFR-02).  Whatever has arrived is taken; the core's staleness
     * timer is what notices if that is nothing. */
    const int n = uart_read_bytes((uart_port_t)KILN_HAL_SUP_UART, out, cap, 0);
    return (n > 0) ? (size_t)n : 0u;
}
