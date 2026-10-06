/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Heat output: two SSR channels and the heat-enable charge pump
 * (HR-06, HR-07, HR-08, HR-12, AD-05, AD-07, SR-02, SR-27).
 *
 * ===========================================================================
 * READ THIS BEFORE CHANGING enable_refresh()
 * ===========================================================================
 * AD-05 and SR-02 rest entirely on one property: **heat enable is a square
 * wave this function generates, one edge per call, and nothing else.**
 *
 * The pin drives an RC charge pump that holds the contactor coil only while it
 * keeps seeing edges.  Stop calling this -- crash, hang, deadlock, a missed
 * deadline, a panic, a power loss -- and the coil de-energises in about a
 * second with no code involved.  That is the whole of SR-02, and it is why
 * SR-14's watchdog requirement costs nothing extra: a watchdog expiry removes
 * heat by the same mechanism as any other way of failing to run.
 *
 * Three changes would each leave this file compiling, the board working on the
 * bench, and the property silently gone:
 *
 *   - setting the pin to a level instead of toggling it;
 *   - handing the toggle to LEDC, MCPWM, RMT or any other peripheral that can
 *     keep producing edges after the CPU has stopped executing;
 *   - calling it from a timer callback rather than from the safety task.
 *
 * The third is the subtle one.  An esp_timer callback runs from a dedicated
 * task that a hung *safety* task does not block, so moving the refresh there
 * would keep the contactor closed while the supervisor was dead.  It must be
 * called by the task that is also evaluating the rules, so that the thing
 * holding the contactor closed is the thing deciding whether it should be.
 * ===========================================================================
 *
 * set_level() is the other half, and belongs to AD-07: it is called from the
 * 10 ms window timer, so it is allocation-free, lock-free and short.
 */
#include "driver/gpio.h"
#include "esp_log.h"

#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

static const char *TAG = "hal_heat";

static const gpio_num_t s_ssr_pin[KILN_HEAT_CHANNELS] = {
    (gpio_num_t)KILN_PIN_SSR1,
    (gpio_num_t)KILN_PIN_SSR2,
};

typedef struct {
    bool     level[KILN_HEAT_CHANNELS];
    uint16_t duty[KILN_HEAT_CHANNELS];
    uint32_t switches[KILN_HEAT_CHANNELS];
    bool     enable_phase;        /* AD-05: the square wave's current half */
    bool     authorised;          /* refreshes are being accepted          */
} heat_t;

static heat_t s_heat;

/* --- port_heat ---------------------------------------------------------- */

static uint8_t heat_channel_count(void *ctx)
{
    (void)ctx;
    /* HR-12: the board populates both. A single-zone kiln (ASM-02) drives them
     * together through kiln_port_heat_set_duty_all(). */
    return KILN_HEAT_CHANNELS;
}

static void heat_set_duty(void *ctx, uint8_t channel, uint16_t permille)
{
    (void)ctx;
    if (channel >= KILN_HEAT_CHANNELS) {
        return;
    }
    /* Telemetry only.  The pin is driven by set_level() from the window timer;
     * publishing a duty here would be a second opinion about the same thing. */
    s_heat.duty[channel] = (permille > KILN_DUTY_MAX) ? KILN_DUTY_MAX : permille;
}

static void heat_set_level(void *ctx, uint8_t channel, bool on)
{
    (void)ctx;
    if (channel >= KILN_HEAT_CHANNELS) {
        return;
    }
    /* Count edges, not calls: the window timer asserts the same level for most
     * of a window, and FR-CUR-13 wants switching operations. */
    if (s_heat.level[channel] != on) {
        s_heat.switches[channel]++;
        s_heat.level[channel] = on;
        gpio_set_level(s_ssr_pin[channel], on ? 1 : 0);
    }
}

static void heat_enable_refresh(void *ctx)
{
    (void)ctx;
    if (!s_heat.authorised) {
        return;
    }
    /* AD-05.  One edge.  See the banner above. */
    s_heat.enable_phase = !s_heat.enable_phase;
    gpio_set_level((gpio_num_t)KILN_PIN_HEAT_EN, s_heat.enable_phase ? 1 : 0);
}

static void heat_drop_contactor(void *ctx)
{
    (void)ctx;
    /* SR-27: stop feeding the pump and park the pin low, so the coil drops on
     * the RC decay rather than waiting for the next refresh that never comes.
     * The verdict budget of NFR-27 allows the decay; what it does not allow is
     * the pump being topped up again, hence the latch. */
    s_heat.authorised   = false;
    s_heat.enable_phase = false;
    gpio_set_level((gpio_num_t)KILN_PIN_HEAT_EN, 0);
}

static void heat_force_off(void *ctx)
{
    /* SR-16's first two steps, in that order: duty to zero at the pins, then
     * heat enable away.  Doing it the other way round would leave the SSRs
     * commanded on across the contactor's drop-out, which is the one moment
     * the contacts are breaking current. */
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        s_heat.duty[c] = 0;
        if (s_heat.level[c]) {
            s_heat.switches[c]++;
            s_heat.level[c] = false;
        }
        gpio_set_level(s_ssr_pin[c], 0);
    }
    heat_drop_contactor(ctx);
}

static bool heat_is_off(void *ctx)
{
    (void)ctx;
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        if (s_heat.level[c]) {
            return false;
        }
    }
    return !s_heat.authorised;
}

static uint32_t heat_switch_count(void *ctx, uint8_t channel)
{
    (void)ctx;
    return (channel < KILN_HEAT_CHANNELS) ? s_heat.switches[channel] : 0u;
}

/* --- construction ------------------------------------------------------- */

void kiln_hal_heat_init(kiln_port_heat_t *out)
{
    if (out == nullptr) {
        return;
    }

    /* HR-08: every heat output has an external pull-down to the de-energised
     * state, and none of these is a strapping pin (see board_pins.h).  The
     * internal pull-down is belt and braces for the window between reset and
     * this call; the external one is what HR-08 actually requires, because an
     * internal pull-down is not configured until the firmware runs. */
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << KILN_PIN_SSR1) | (1ULL << KILN_PIN_SSR2) |
                      (1ULL << KILN_PIN_HEAT_EN);
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    (void)gpio_config(&io);

    /* Safe state before anything can ask otherwise (SR-21, NFR-09). */
    gpio_set_level((gpio_num_t)KILN_PIN_SSR1, 0);
    gpio_set_level((gpio_num_t)KILN_PIN_SSR2, 0);
    gpio_set_level((gpio_num_t)KILN_PIN_HEAT_EN, 0);

    /* Counters survive this call: FR-CUR-13 counts operations of the physical
     * device, and a re-init is not a new contactor.  The persisted totals come
     * from port_counters; these are the since-boot deltas the app reconciles. */
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        s_heat.level[c] = false;
        s_heat.duty[c]  = 0;
    }
    s_heat.enable_phase = false;
    s_heat.authorised   = true;

    out->ctx             = &s_heat;
    out->channel_count   = heat_channel_count;
    out->set_duty        = heat_set_duty;
    out->set_level       = heat_set_level;
    out->enable_refresh  = heat_enable_refresh;
    out->drop_contactor  = heat_drop_contactor;
    out->force_off       = heat_force_off;
    out->is_off          = heat_is_off;
    out->switch_count    = heat_switch_count;

    ESP_LOGI(TAG, "SSR1=%d SSR2=%d HEAT_EN=%d (charge pump, toggled per safety cycle)",
             KILN_PIN_SSR1, KILN_PIN_SSR2, KILN_PIN_HEAT_EN);
}

/* SR-18: an acknowledged fault has to be able to re-arm the output, or a kiln
 * would need a power cycle after every cleared fault.  Separate from init so
 * that re-arming cannot be confused with reconfiguring the pins. */
void kiln_hal_heat_rearm(void)
{
    s_heat.authorised = true;
}
