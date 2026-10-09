/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The small discrete adapters: lid interlock sense and buzzer
 * (SWR-SAF-31, SYS-HW-21, SYS-HW-09, SWR-SAF-20).
 *
 * Grouped because each is a handful of lines around one pin, and three files of
 * twenty lines would be three files to find rather than one.
 */
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kiln_core/alarmptn.h"
#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "hal_io";

/* --- lid interlock (SWR-SAF-31, SYS-HW-21) --------------------------------------- */
/*
 * What this pin reads is not the switch: SYS-HW-21 puts the contacts in the coil
 * circuit, and R30/R31 divide the node *after* them.  So a high level means
 * "the coil supply is live", which is true exactly when the lid is shut.
 *
 * That has a consequence worth stating.  The sense is downstream of the
 * contacts, so it reports the state of the interlock chain rather than of the
 * switch alone: a blown coil fuse would also read as "open".  For SWR-SAF-31 that
 * is the right answer -- the rule asks whether heat can reach the kiln -- but
 * the fault text says "door", and an installer chasing a fuse should know the
 * input cannot tell them apart.
 */
typedef struct {
    bool fitted;
} door_t;

door_t s_door;

bool door_is_open(void *ctx)
{
    (void)ctx;
    /* port_door.h: an adapter that cannot read the pin reports open.  A pin
     * configured as an input with a pull-down reads low when nothing drives
     * it, which is "open", so the failure direction is already the safe one. */
    return gpio_get_level((gpio_num_t)KILN_PIN_LID_SENSE) == 0;
}

bool door_is_present(void *ctx)
{
    (void)ctx;
    return (static_cast<door_t *>(ctx) != nullptr) ? s_door.fitted : false;
}

} // namespace

void kiln_hal_door_init(kiln_port_door_t *out, bool interlock_fitted)
{
    if (out == nullptr) {
        return;
    }
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << (unsigned)KILN_PIN_LID_SENSE;
    io.mode         = GPIO_MODE_INPUT;
    /* Pull-down, so an unfitted or broken sense line reads "open" and withholds
     * heat rather than reading "shut" and permitting it. */
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    (void)gpio_config(&io);

    s_door.fitted = interlock_fitted;
    out->ctx        = &s_door;
    out->is_open    = door_is_open;
    out->is_present = door_is_present;

    ESP_LOGI(TAG, "lid sense on IO%d, interlock %s", KILN_PIN_LID_SENSE,
             interlock_fitted ? "fitted" : "NOT fitted (warning 113)");
}

namespace {

/* --- buzzer (SYS-HW-09, SWR-SAF-20) ---------------------------------------------- */
/*
 * The rhythms are in kiln_core/alarmptn, where they are host-tested; this is
 * the half that cannot be: a pin and a one-shot timer.
 *
 * Driven by esp_timer rather than by the control task, because the pattern must
 * keep sounding while the operator is doing something else, and because a fault
 * pattern that stopped when a task got busy would be a fault nobody heard.
 */
typedef struct {
    esp_timer_handle_t   timer;
    kiln_alarm_seq_t     seq;
} alarm_t;

alarm_t s_alarm;

void alarm_tick(void *arg)
{
    alarm_t *a = static_cast<alarm_t *>(arg);
    const kiln_alarm_step_t s = kiln_alarm_seq_next(&a->seq);

    gpio_set_level((gpio_num_t)KILN_PIN_ALARM, s.on ? 1 : 0);
    if (s.hold_ms > 0u) {
        (void)esp_timer_start_once(a->timer, (uint64_t)s.hold_ms * 1000u);
    }
    /* hold_ms == 0 means nothing further is due, and the level set above is
     * already the silent one.  No timer is armed, so the buzzer stays quiet
     * until something sets a pattern. */
}

void alarm_set(void *ctx, kiln_alarm_pattern_t pattern)
{
    alarm_t *a = static_cast<alarm_t *>(ctx);
    if (a == nullptr) {
        return;
    }
    if (a->seq.pattern == pattern) {
        return;                       /* re-asserting must not restart the beat */
    }
    (void)esp_timer_stop(a->timer);
    kiln_alarm_seq_begin(&a->seq, pattern);

    /* Start loud: the first interval is applied now, not one period away. */
    alarm_tick(a);
}

} // namespace

void kiln_hal_alarm_init(kiln_port_alarm_t *out)
{
    if (out == nullptr) {
        return;
    }
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << (unsigned)KILN_PIN_ALARM;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.intr_type    = GPIO_INTR_DISABLE;
    (void)gpio_config(&io);
    gpio_set_level((gpio_num_t)KILN_PIN_ALARM, 0);

    esp_timer_create_args_t args = {};
    args.callback        = alarm_tick;
    args.arg             = &s_alarm;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name            = "alarm";
    (void)esp_timer_create(&args, &s_alarm.timer);

    kiln_alarm_seq_begin(&s_alarm.seq, KILN_ALARM_OFF);

    out->ctx = &s_alarm;
    out->set = alarm_set;
}
