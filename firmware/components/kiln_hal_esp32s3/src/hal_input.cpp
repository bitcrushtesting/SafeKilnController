/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Rotary encoder with push button (HR-05, FR-HMI-09).
 *
 * HR-05 requires the encoder to be read **by hardware** -- the pulse counter
 * unit -- rather than polled in software.  That is not a performance
 * preference: a detent turned while the control task is busy would be lost by
 * a polling reader, and an operator who turns the knob and sees nothing turns
 * it further, which is how a setpoint ends up somewhere nobody intended.  PCNT
 * counts the edges whether or not anybody is looking.
 *
 * The button is read in software, which HR-05 permits: a press is tens of
 * milliseconds wide and the debounce has to be in software anyway.
 */
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

static const char *TAG = "hal_input";

/* A detent of these encoders is one full quadrature cycle, which 4x decoding
 * counts as four edges.  Dividing by four means one click of the knob is one
 * event, which is what the operator means by one click. */
#define ENC_COUNTS_PER_DETENT   4

/* FR-HMI-09 and port_input.h: a long press is 400 ms. */
#define BTN_LONG_PRESS_US       (400 * 1000)
/* Contact bounce on a panel encoder is a few milliseconds; 20 ms is generous
 * and still far below the shortest press a person can make. */
#define BTN_DEBOUNCE_US         (20 * 1000)

typedef struct {
    pcnt_unit_handle_t unit;
    int                last_count;

    /* button */
    bool     pressed;          /* debounced state                     */
    bool     raw_last;
    int64_t  raw_changed_us;
    int64_t  pressed_at_us;
    bool     long_sent;        /* the long press has already been reported */
} input_t;

static input_t s_in;

static kiln_input_event_t poll_rotation(input_t *in)
{
    int count = 0;
    if (pcnt_unit_get_count(in->unit, &count) != ESP_OK) {
        return KILN_INPUT_NONE;
    }

    const int delta = count - in->last_count;
    if (delta >= ENC_COUNTS_PER_DETENT) {
        in->last_count += ENC_COUNTS_PER_DETENT;
        return KILN_INPUT_CW;
    }
    if (delta <= -ENC_COUNTS_PER_DETENT) {
        in->last_count -= ENC_COUNTS_PER_DETENT;
        return KILN_INPUT_CCW;
    }
    /* A partial detent is left on the counter rather than discarded: the knob
     * may be resting between positions, and rounding it away would make slow
     * turns disappear. */
    return KILN_INPUT_NONE;
}

static kiln_input_event_t poll_button(input_t *in)
{
    /* Active low: the button pulls to ground against the internal pull-up. */
    const bool    raw = gpio_get_level((gpio_num_t)KILN_PIN_ENC_BTN) == 0;
    const int64_t now = esp_timer_get_time();

    if (raw != in->raw_last) {
        in->raw_last        = raw;
        in->raw_changed_us  = now;
        return KILN_INPUT_NONE;      /* wait for it to settle */
    }
    if ((now - in->raw_changed_us) < BTN_DEBOUNCE_US) {
        return KILN_INPUT_NONE;
    }

    if (raw && !in->pressed) {
        in->pressed       = true;
        in->pressed_at_us = now;
        in->long_sent     = false;
        return KILN_INPUT_NONE;      /* nothing yet: it may become a long press */
    }

    if (raw && in->pressed && !in->long_sent &&
        (now - in->pressed_at_us) >= BTN_LONG_PRESS_US) {
        /* Reported while the button is still down, deliberately: a long press
         * that only arrived on release would make the operator hold the knob
         * wondering whether it had worked. */
        in->long_sent = true;
        return KILN_INPUT_LONG_PRESS;
    }

    if (!raw && in->pressed) {
        in->pressed = false;
        /* A short press is reported on release, because until then it might
         * still have become a long one. */
        return in->long_sent ? KILN_INPUT_NONE : KILN_INPUT_PRESS;
    }
    return KILN_INPUT_NONE;
}

static kiln_input_event_t input_poll(void *ctx)
{
    input_t *in = (input_t *)ctx;
    if (in == nullptr) {
        return KILN_INPUT_NONE;
    }
    /* Rotation first: a knob being turned is the more time-critical of the two,
     * and the button cannot be missed by one cycle's delay. */
    const kiln_input_event_t rot = poll_rotation(in);
    if (rot != KILN_INPUT_NONE) {
        return rot;
    }
    return poll_button(in);
}

kiln_err_t kiln_hal_input_init(kiln_port_input_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    /* The limits are wide and symmetric: the HMI consumes deltas, so the
     * counter is free to run and only wraps after half a million detents.
     *
     * As elsewhere in this component: the IDF idiom is zero then assign, and
     * this struct's clock source enum has no zero enumerator, so the zero is
     * briefly an invalid value. The driver fills it from the SoC default.
     * NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
    pcnt_unit_config_t uc = {};
    uc.high_limit = 32767;
    uc.low_limit  = -32768;
    if (pcnt_new_unit(&uc, &s_in.unit) != ESP_OK) {
        return KILN_ERR_IO;
    }

    /* Glitch filter in hardware, which is the other half of why HR-05 wants
     * PCNT: contact bounce on the quadrature lines is filtered before it can
     * be counted, rather than after it has already moved a setpoint. */
    pcnt_glitch_filter_config_t filt = {};
    filt.max_glitch_ns = 1000;
    if (pcnt_unit_set_glitch_filter(s_in.unit, &filt) != ESP_OK) {
        return KILN_ERR_IO;
    }

    /* 4x quadrature decoding: each channel counts edges on one line while the
     * other line selects the direction. */
    pcnt_chan_config_t ca = {};
    ca.edge_gpio_num  = KILN_PIN_ENC_A;
    ca.level_gpio_num = KILN_PIN_ENC_B;
    pcnt_channel_handle_t cha = nullptr;
    if (pcnt_new_channel(s_in.unit, &ca, &cha) != ESP_OK) {
        return KILN_ERR_IO;
    }

    pcnt_chan_config_t cb = {};
    cb.edge_gpio_num  = KILN_PIN_ENC_B;
    cb.level_gpio_num = KILN_PIN_ENC_A;
    pcnt_channel_handle_t chb = nullptr;
    if (pcnt_new_channel(s_in.unit, &cb, &chb) != ESP_OK) {
        return KILN_ERR_IO;
    }

    (void)pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                            PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    (void)pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                             PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    (void)pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                            PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    (void)pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                             PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    if (pcnt_unit_enable(s_in.unit) != ESP_OK ||
        pcnt_unit_clear_count(s_in.unit) != ESP_OK ||
        pcnt_unit_start(s_in.unit) != ESP_OK) {
        return KILN_ERR_IO;
    }

    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << KILN_PIN_ENC_BTN;
    btn.mode         = GPIO_MODE_INPUT;
    btn.pull_up_en   = GPIO_PULLUP_ENABLE;      /* active low */
    btn.pull_down_en = GPIO_PULLDOWN_DISABLE;
    btn.intr_type    = GPIO_INTR_DISABLE;
    (void)gpio_config(&btn);

    s_in.last_count     = 0;
    s_in.pressed        = false;
    s_in.raw_last       = false;
    s_in.raw_changed_us = esp_timer_get_time();

    out->ctx  = &s_in;
    out->poll = input_poll;

    ESP_LOGI(TAG, "encoder A=%d B=%d BTN=%d, PCNT 4x with 1 us glitch filter",
             KILN_PIN_ENC_A, KILN_PIN_ENC_B, KILN_PIN_ENC_BTN);
    return KILN_OK;
}
