/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SSD1306 128x64 monochrome OLED over I2C (HR-04, FR-HMI-14).
 *
 * The port transfers a framebuffer and nothing else: the HMI renders into it,
 * so screens are verifiable by golden-image comparison on the host with no
 * display present.  Keeping the font and the layout out of this file is what
 * makes that possible, and is the same reason the log ring lives in the core.
 *
 * FR-HMI-14 is the governing requirement here: if the display fails to
 * initialise or stops acknowledging, the system logs it and **keeps
 * controlling the kiln**.  So nothing in this file returns an error that could
 * be mistaken for a reason to stop, and `available()` exists so the
 * application can raise warning 104 rather than infer a fault.
 */
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

static const char *TAG = "hal_display";

/* SSD1306 commands, named rather than spelled as magic bytes at the call. */
enum : uint8_t {
    SSD_DISPLAY_OFF      = 0xAE,
    SSD_DISPLAY_ON       = 0xAF,
    SSD_SET_CONTRAST     = 0x81,
    SSD_SET_MEMORY_MODE  = 0x20,
    SSD_SET_COL_ADDR     = 0x21,
    SSD_SET_PAGE_ADDR    = 0x22,
    SSD_SET_START_LINE   = 0x40,
    SSD_SEG_REMAP_ON     = 0xA1,
    SSD_COM_SCAN_DEC     = 0xC8,
    SSD_SET_MUX_RATIO    = 0xA8,
    SSD_SET_DISPLAY_OFFS = 0xD3,
    SSD_SET_COM_PINS     = 0xDA,
    SSD_SET_CLOCK_DIV    = 0xD5,
    SSD_SET_PRECHARGE    = 0xD9,
    SSD_SET_VCOM_DESEL   = 0xDB,
    SSD_ENTIRE_ON_RAM    = 0xA4,
    SSD_NORMAL_DISPLAY   = 0xA6,
    SSD_CHARGE_PUMP      = 0x8D,
};

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool                    ok;        /* FR-HMI-14: last transfer succeeded */
    uint32_t                failures;
} disp_t;

static disp_t s_disp;

/* A command stream is a 0x00 control byte then the bytes themselves. */
static bool send_cmds(disp_t *d, const uint8_t *cmds, size_t n)
{
    uint8_t buf[24];
    if (n + 1u > sizeof(buf)) {
        return false;
    }
    buf[0] = 0x00;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(d->dev, buf, n + 1u, 50) == ESP_OK;
}

static kiln_err_t disp_present(void *ctx, const uint8_t *fb, size_t len)
{
    disp_t *d = (disp_t *)ctx;
    if ((d == nullptr) || (fb == nullptr) || (d->dev == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (len != KILN_DISPLAY_BYTES) {
        return KILN_ERR_RANGE;
    }

    /* Address the whole panel, then stream it.  Setting the window every frame
     * costs five bytes and removes a class of bug where a partial update leaves
     * the pointer somewhere the next frame does not expect. */
    const uint8_t window[] = {
        SSD_SET_COL_ADDR,  0, KILN_DISPLAY_W - 1,
        SSD_SET_PAGE_ADDR, 0, KILN_DISPLAY_PAGES - 1,
    };
    if (!send_cmds(d, window, sizeof(window))) {
        d->ok = false;
        d->failures++;
        return KILN_ERR_IO;
    }

    /* 0x40 marks the rest of the transfer as display data.  One transmit for
     * the whole frame: 1025 bytes at 400 kHz is about 25 ms, and NFR-02 bounds
     * non-safety work at 50 ms, so this must not be split into page writes
     * that could interleave with the control cycle. */
    static uint8_t frame[KILN_DISPLAY_BYTES + 1];
    frame[0] = 0x40;
    memcpy(&frame[1], fb, len);

    if (i2c_master_transmit(d->dev, frame, len + 1u, 100) != ESP_OK) {
        d->ok = false;
        d->failures++;
        /* FR-HMI-14: report it and let the caller warn; do not escalate. */
        return KILN_ERR_IO;
    }
    d->ok = true;
    return KILN_OK;
}

static kiln_err_t disp_set_contrast(void *ctx, uint8_t contrast)
{
    disp_t *d = (disp_t *)ctx;
    if ((d == nullptr) || (d->dev == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    /* FR-HMI-12's dim timeout is the caller's; this is only the knob. */
    const uint8_t cmds[] = { SSD_SET_CONTRAST, contrast };
    return send_cmds(d, cmds, sizeof(cmds)) ? KILN_OK : KILN_ERR_IO;
}

static bool disp_available(void *ctx)
{
    const disp_t *d = (const disp_t *)ctx;
    return (d != nullptr) && d->ok;
}

kiln_err_t kiln_hal_display_init(kiln_port_display_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus = {};
    bus.i2c_port                   = I2C_NUM_0;
    bus.sda_io_num                 = (gpio_num_t)KILN_PIN_I2C_SDA;
    bus.scl_io_num                 = (gpio_num_t)KILN_PIN_I2C_SCL;
    bus.clk_source                 = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt          = 7;
    bus.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus, &s_disp.bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return KILN_ERR_IO;
    }

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = KILN_HAL_OLED_ADDR;
    dev.scl_speed_hz    = 400 * 1000;

    if (i2c_master_bus_add_device(s_disp.bus, &dev, &s_disp.dev) != ESP_OK) {
        return KILN_ERR_IO;
    }

    /* The initialisation sequence from the SSD1306 datasheet for a 128x64
     * panel with the internal charge pump.  Order matters: the charge pump has
     * to be enabled before the display is turned on, or the panel stays dark
     * while every command succeeds. */
    static const uint8_t init[] = {
        SSD_DISPLAY_OFF,
        SSD_SET_CLOCK_DIV,    0x80,
        SSD_SET_MUX_RATIO,    KILN_DISPLAY_H - 1,
        SSD_SET_DISPLAY_OFFS, 0x00,
        SSD_SET_START_LINE,
        SSD_CHARGE_PUMP,      0x14,
        SSD_SET_MEMORY_MODE,  0x00,          /* horizontal addressing */
        SSD_SEG_REMAP_ON,
        SSD_COM_SCAN_DEC,
        SSD_SET_COM_PINS,     0x12,
        SSD_SET_CONTRAST,     0xCF,
        SSD_SET_PRECHARGE,    0xF1,
        SSD_SET_VCOM_DESEL,   0x40,
        SSD_ENTIRE_ON_RAM,
        SSD_NORMAL_DISPLAY,
        SSD_DISPLAY_ON,
    };
    /* Sent in chunks, because send_cmds keeps a small stack buffer and a
     * nineteen-byte burst is not worth a heap allocation. */
    bool ok = true;
    for (size_t i = 0; i < sizeof(init) && ok; i += 8u) {
        const size_t n = ((sizeof(init) - i) < 8u) ? (sizeof(init) - i) : 8u;
        ok = send_cmds(&s_disp, &init[i], n);
    }
    s_disp.ok = ok;

    out->ctx          = &s_disp;
    out->present      = disp_present;
    out->set_contrast = disp_set_contrast;
    out->available    = disp_available;

    if (!ok) {
        /* FR-HMI-14: a display that will not initialise is a warning, not a
         * reason to refuse to run a kiln. */
        ESP_LOGE(TAG, "SSD1306 at 0x%02x did not acknowledge; continuing without it",
                 KILN_HAL_OLED_ADDR);
        return KILN_OK;
    }
    ESP_LOGI(TAG, "SSD1306 128x64 at 0x%02x on SDA=%d SCL=%d",
             KILN_HAL_OLED_ADDR, KILN_PIN_I2C_SDA, KILN_PIN_I2C_SCL);
    return KILN_OK;
}
