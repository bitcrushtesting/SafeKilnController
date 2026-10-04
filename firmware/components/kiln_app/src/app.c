/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_app/app.h"

/* --- configuration fan-out --------------------------------------------- */

/* One configuration struct, several core components with their own.  Doing the
 * translation in one function means a configuration item cannot reach one
 * consumer and miss another -- which is the failure mode FR-CFG-02's long list
 * invites. */
static void push_config(kiln_app_t *app)
{
    const kiln_config_t *c = &app->cfg;

    kiln_tempfilt_cfg_t tf = {
        .offset_c      = c->cal_offset_c,
        .gain          = c->cal_gain,
        .filter_tau_s  = c->filter_tau_s,
        .rate_window_s = c->rate_window_s,
    };
    kiln_tempfilt_reconfigure(&app->filt, &tf);

    kiln_tempfilt_cfg_t cf = {
        .offset_c      = c->case_cal_offset_c,
        .gain          = c->case_cal_gain,
        .filter_tau_s  = c->filter_tau_s,
        .rate_window_s = c->rate_window_s,
    };
    kiln_tempfilt_reconfigure(&app->case_filt, &cf);

    kiln_pid_set_gains(&app->pid, c->kp, c->ki, c->kd);
    kiln_pid_set_duty_max(&app->pid, c->duty_max_permille);

    kiln_window_cfg_t wc;
    kiln_window_cfg_defaults(&wc);
    wc.window_ms           = c->window_ms;
    wc.min_on_ms           = c->min_on_ms;
    wc.min_off_ms          = c->min_off_ms;
    wc.preserve_off_window = c->current_enabled;   /* see kiln_window_cfg_t */
    (void)kiln_window_reconfigure(&app->win, &wc);

    kiln_safety_cfg_t sc;
    kiln_safety_cfg_defaults(&sc);
    sc.max_temp_c               = c->max_temp_c;
    sc.max_case_temp_c          = c->max_case_temp_c;
    sc.overtemp_margin_c        = c->overtemp_margin_c;
    sc.runaway_duty_permille    = c->runaway_duty_permille;
    sc.runaway_window_s         = c->runaway_window_s;
    sc.runaway_min_rate_c_per_h = c->runaway_min_rate_c_per_h;
    sc.fail_on_threshold_a      = c->fail_on_threshold_a;
    sc.fail_on_windows          = (uint8_t)c->fail_on_windows;
    sc.fail_off_fraction        = c->fail_off_fraction;
    sc.fail_off_window_s        = c->fail_off_window_s;
    sc.weld_wait_s              = c->weld_wait_s;
    sc.weld_verdict_s           = c->weld_wait_s + 1.0f;   /* NFR-27 */
    sc.deviation_warn_frac      = c->deviation_warn_frac;
    sc.deviation_fault_frac     = c->deviation_fault_frac;
    sc.overcurrent_a            = c->overcurrent_a;
    sc.contactor_life_ops       = c->contactor_life_ops;
    sc.ssr_life_ops             = c->ssr_life_ops;
    kiln_safety_cfg_set_nominal_current(&sc, c->nominal_a);
    sc.overcurrent_a            = c->overcurrent_a;   /* explicit beats derived */
    kiln_safety_reconfigure(&app->safety, &sc);

    kiln_current_cfg_t cc;
    kiln_current_cfg_defaults(&cc);
    cc.enabled          = c->current_enabled && app->ports.current != NULL;
    cc.sample_rate_hz   = c->current_sample_rate_hz;
    cc.mains_hz         = c->mains_hz;
    cc.settle_ms        = c->current_settle_ms;
    cc.ct_a_per_v       = c->ct_a_per_v;
    cc.cal_gain         = c->current_cal_gain;
    cc.zero_offset_a    = c->current_zero_offset_a;
    cc.mains_v          = c->mains_v;
    cc.nominal_a        = c->nominal_a;
    cc.element_tc_per_c = c->element_tc_per_c;
    (void)kiln_current_reconfigure(&app->cur, &cc);
}

kiln_err_t kiln_app_init(kiln_app_t *app, const kiln_app_ports_t *ports,
                         const kiln_config_t *cfg)
{
    if (!app || !ports || !ports->tc || !ports->heat) return KILN_ERR_INVALID_ARG;

    memset(app, 0, sizeof(*app));
    app->ports = *ports;

    if (cfg) {
        app->cfg = *cfg;
    } else {
        kiln_config_defaults(&app->cfg);
    }

    const kiln_tempfilt_cfg_t tf0 = { .gain = 1.0f };
    kiln_tempfilt_init(&app->filt, &tf0);
    kiln_tempfilt_init(&app->case_filt, &tf0);

    const kiln_pid_cfg_t pid0 = { .duty_max_permille = KILN_DUTY_MAX };
    kiln_pid_init(&app->pid, &pid0);

    kiln_window_cfg_t wc;
    kiln_window_cfg_defaults(&wc);
    (void)kiln_window_init(&app->win, &wc);

    kiln_safety_cfg_t sc;
    kiln_safety_cfg_defaults(&sc);
    kiln_safety_init(&app->safety, &sc);

    kiln_current_cfg_t cc;
    kiln_current_cfg_defaults(&cc);
    (void)kiln_current_init(&app->cur, &cc);

    push_config(app);

    app->state       = KILN_STATE_IDLE;
    app->fault       = KILN_FAULT_NONE;
    app->next_run_id = 1;
    app->kiln_c      = app->cfg.max_temp_c > 0.0f ? 20.0f : 20.0f;
    app->case_c      = 20.0f;

    /* FR-TUN-11: gains at their factory values are not gains for *this* kiln. */
    if (app->cfg.kp <= 0.0f) {
        app->warnings |= KILN_WARN_BIT(KILN_WARN_GAINS_UNTUNED);
    }

    if (app->ports.current && app->ports.current->configure) {
        (void)app->ports.current->configure(app->ports.current->ctx, 0,
                                            app->cfg.current_sample_rate_hz);
    }
    if (app->ports.counters && app->ports.counters->load) {
        (void)app->ports.counters->load(app->ports.counters->ctx, &app->counters);
    }
    if (app->ports.tc->configure) {
        (void)app->ports.tc->configure(app->ports.tc->ctx,
                                       (kiln_tc_type_t)app->cfg.tc_type,
                                       (uint8_t)app->cfg.line_filter_hz);
    }
    if (app->ports.case_tc && app->ports.case_tc->configure) {
        (void)app->ports.case_tc->configure(app->ports.case_tc->ctx,
                                            (kiln_tc_type_t)app->cfg.case_tc_type,
                                            (uint8_t)app->cfg.line_filter_hz);
    }
    return KILN_OK;
}

kiln_err_t kiln_app_apply_config(kiln_app_t *app, const kiln_config_t *cfg,
                                 const kiln_cfg_item_t **bad)
{
    if (!app || !cfg) return KILN_ERR_INVALID_ARG;

    const bool running = (app->state == KILN_STATE_RUNNING ||
                          app->state == KILN_STATE_PAUSED  ||
                          app->state == KILN_STATE_MANUAL  ||
                          app->state == KILN_STATE_AUTOTUNE);

    const kiln_err_t e = kiln_config_apply(&app->cfg, cfg, running, bad);
    if (e != KILN_OK) return e;

    push_config(app);
    return KILN_OK;
}

/* --- acquisition (FR-ACQ) ---------------------------------------------- */

static void read_channel(const kiln_port_tc_t *port, kiln_tempfilt_t *filt,
                         float dt_s, float *out_c, float *out_raw_c, float *out_cj_c,
                         uint16_t *out_bits, bool *out_valid)
{
    kiln_tc_reading_t r = {0};

    if (!port || !port->read) {
        *out_bits  = KILN_TC_FAULT_COMMS;
        *out_valid = false;
        return;
    }

    const kiln_err_t e = port->read(port->ctx, &r);
    if (e != KILN_OK && r.fault_bits == 0) r.fault_bits = KILN_TC_FAULT_COMMS;

    *out_bits = r.fault_bits;
    if (out_cj_c) *out_cj_c = r.cj_c;

    if (r.fault_bits != 0) {
        /* FR-ACQ-12: the previous reading stands for the grace period, but it is
         * marked invalid so no rule treats it as a measurement. */
        *out_valid = false;
        return;
    }

    /* NFR-17: a front end that answers with a NaN is a front end fault. */
    if (!kiln_tempfilt_push(filt, r.temp_c, dt_s)) {
        *out_bits  = KILN_TC_FAULT_COMMS;
        *out_valid = false;
        return;
    }

    *out_c     = kiln_tempfilt_filt(filt);
    if (out_raw_c) *out_raw_c = kiln_tempfilt_raw(filt);
    *out_valid = true;
}

void kiln_app_acquire_cycle(kiln_app_t *app, float dt_s)
{
    if (!app) return;

    read_channel(app->ports.tc, &app->filt, dt_s,
                 &app->kiln_c, &app->kiln_raw_c, &app->cj_c,
                 &app->tc_fault_bits, &app->kiln_valid);
    app->rate_c_per_h = kiln_tempfilt_rate(&app->filt);

    if (app->ports.case_tc && app->cfg.case_present) {
        read_channel(app->ports.case_tc, &app->case_filt, dt_s,
                     &app->case_c, NULL, NULL,
                     &app->case_fault_bits, &app->case_valid);
    } else {
        app->case_valid     = false;
        app->case_fault_bits = 0;
    }
}

/* --- the 10 ms path: SSR pin and gated current sampling ----------------- */

/* AD-07 and AD-17 in one function, because they are one mechanism: the pin level
 * *is* the window phase the sampler has to gate on, and anything that learned it
 * second-hand could be a tick out of date. */
void kiln_app_window_tick(kiln_app_t *app, uint32_t dt_ms)
{
    if (!app) return;

    const kiln_port_heat_t *h = app->ports.heat;
    const uint16_t duty = app->heat_authorised ? app->duty_request : 0u;

    const bool on = kiln_window_tick(&app->win, duty, app->heat_authorised);

    if (h->set_level) {
        for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
            h->set_level(h->ctx, ch, on);
        }
    }
    if (h->set_duty) {
        for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
            h->set_duty(h->ctx, ch, duty);
        }
    }

    /* FR-CUR-13: count what the pin actually did, not what was asked for. */
    if (app->ports.counters && app->ports.counters->add_ssr_ops) {
        for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
            const uint32_t seen = h->switch_count ? h->switch_count(h->ctx, ch)
                                                  : app->win.switch_count;
            if (seen > app->ssr_switch_seen[ch]) {
                const uint32_t delta = seen - app->ssr_switch_seen[ch];
                app->ports.counters->add_ssr_ops(app->ports.counters->ctx, ch, delta);
                /* And the app's own view, which is what SR-30 is evaluated
                 * against and what the run record carries: the adapter
                 * accumulates for flash, it is not a readable register. */
                app->counters.ssr_ops[ch] += delta;
                app->ssr_switch_seen[ch]   = seen;
            }
        }
    }

    const kiln_port_current_t *cp = app->ports.current;
    if (!cp) return;

    /* Collect a burst that has run.  Polled, never waited on (FR-CUR-14). */
    if (app->cur_burst_pending && cp->read_burst) {
        kiln_cur_burst_t b = {0};
        const kiln_err_t e = cp->read_burst(cp->ctx, 0, &b);
        if (e == KILN_OK) {
            app->cur_burst_pending = false;
            if (kiln_current_push_burst(&app->cur, &b) == KILN_OK) {
                app->cur_fresh = true;
                app->cur_deviation_valid =
                    kiln_current_deviation(&app->cur, &app->cur_deviation);
            }
        } else if (e != KILN_ERR_BUSY) {
            app->cur_burst_pending = false;
        }
    }

    const uint32_t remaining = kiln_window_level_remaining_ms(&app->win, duty);
    uint16_t n = 0;
    const kiln_cur_action_t act =
        kiln_current_tick(&app->cur,
                          on ? KILN_CUR_WINDOW_ON : KILN_CUR_WINDOW_OFF,
                          remaining, dt_ms, &n);

    if (act == KILN_CUR_ACT_START_BURST && cp->start_burst && !app->cur_burst_pending) {
        if (cp->start_burst(cp->ctx, 0, on ? KILN_CUR_WINDOW_ON : KILN_CUR_WINDOW_OFF,
                            n) == KILN_OK) {
            app->cur_burst_pending = true;
        }
    } else if (act == KILN_CUR_ACT_ABORT) {
        if (cp->abort_burst) cp->abort_burst(cp->ctx, 0);
        app->cur_burst_pending = false;
    }
}

/* --- control (FR-CTL) -------------------------------------------------- */

static void finish_run(kiln_app_t *app, kiln_run_end_t reason, kiln_fault_t fault);

void kiln_app_control_cycle(kiln_app_t *app, float dt_s)
{
    if (!app || !(dt_s > 0.0f)) return;

    kiln_current_note_plant(&app->cur, app->kiln_c, app->duty_request, dt_s);

    switch (app->state) {
    case KILN_STATE_RUNNING: {
        app->run_elapsed_s += dt_s;
        (void)kiln_setpoint_tick(&app->sp, app->kiln_c, dt_s);

        if (kiln_setpoint_finished(&app->sp)) {
            finish_run(app, KILN_END_COMPLETE, KILN_FAULT_NONE);
            app->duty_request = 0;
            break;
        }

        app->heat_allowed = kiln_setpoint_heat_allowed(&app->sp);

        /* FR-CTL-13: a cooling ramp is passive.  Decided by the setpoint
         * generator rather than by the sign of the PID error, so a lagging kiln
         * cannot talk the controller into heating through a cooling segment. */
        if (!app->heat_allowed) {
            app->duty_request = 0;
            kiln_pid_reset(&app->pid);
        } else {
            app->duty_request = kiln_pid_update(&app->pid,
                                                kiln_setpoint_value(&app->sp),
                                                app->kiln_c, dt_s);
        }
        break;
    }

    case KILN_STATE_MANUAL:
        app->heat_allowed = true;
        app->duty_request = app->duty_manual;
        break;

    case KILN_STATE_AUTOTUNE:
        app->heat_allowed = true;
        app->duty_request = kiln_autotune_tick(&app->tune, app->kiln_c,
                                               app->rate_c_per_h, dt_s);
        if (kiln_autotune_done(&app->tune)) {
            if (kiln_autotune_succeeded(&app->tune)) {
                /* FR-TUN-09: present, do not store.  The operator decides. */
                app->state = KILN_STATE_COMPLETE;
            } else {
                finish_run(app, KILN_END_FAULT, app->tune.fail_reason);
            }
            app->duty_request = 0;
        }
        break;

    case KILN_STATE_PAUSED:
    case KILN_STATE_IDLE:
    case KILN_STATE_COMPLETE:
    case KILN_STATE_FAULT:
    default:
        app->duty_request = 0;
        app->heat_allowed = false;
        break;
    }

    /* FR-CTL-11 / warning 102, recomputed: it is a live condition. */
    if (kiln_setpoint_holdback(&app->sp) && app->state == KILN_STATE_RUNNING) {
        app->warnings |= KILN_WARN_BIT(KILN_WARN_HOLDBACK);
    } else {
        app->warnings &= ~KILN_WARN_BIT(KILN_WARN_HOLDBACK);
    }

    if (app->kiln_c > app->record.peak_c) app->record.peak_c = app->kiln_c;
    app->record.energy_wh    = (float)kiln_current_energy_wh(&app->cur);
    app->record.current_ref_a = kiln_current_ref(&app->cur);
}

/* --- safety (SR-*) ----------------------------------------------------- */

static bool is_heating_state(kiln_state_t s)
{
    return s == KILN_STATE_RUNNING || s == KILN_STATE_MANUAL ||
           s == KILN_STATE_AUTOTUNE;
}

static void build_safety_input(const kiln_app_t *app, kiln_safety_input_t *in)
{
    memset(in, 0, sizeof(*in));

    in->kiln_c                  = app->kiln_c;
    in->case_c                  = app->case_c;
    in->rate_c_per_h            = app->rate_c_per_h;
    in->setpoint_c              = kiln_setpoint_value(&app->sp);
    in->duty_permille           = app->duty_request;
    in->tc_fault_bits           = app->tc_fault_bits;
    in->case_fault_bits         = app->case_fault_bits;
    in->case_present            = app->cfg.case_present;
    in->heating_active          = is_heating_state(app->state);
    in->kiln_valid              = app->kiln_valid;
    in->case_valid              = app->case_valid;

    /* FR-CUR-11 against FR-CUR-12, and the distinction matters: monitoring is
     * *on* whenever it is configured on, even if the channel is not answering.
     *
     * Folding "the CT is absent" into "monitoring is off" would downgrade a
     * missing transformer from fault 26 to warning 111 -- turning a hardware
     * failure into a notice that the operator had chosen to fire without
     * electrical cover, which they had not.  So the front end's own view is fed
     * in as a CT fault, and warning 111 is reserved for a deliberate choice. */
    const bool port_ok = app->ports.current &&
                         (!app->ports.current->present ||
                          app->ports.current->present(app->ports.current->ctx, 0));
    in->current_monitoring      = app->cfg.current_enabled;
    in->current_fresh           = app->cur_fresh;
    in->current_a               = kiln_current_amps(&app->cur);
    in->current_ref_a           = kiln_current_ref(&app->cur);
    in->current_flags           = kiln_current_flags(&app->cur);
    if (app->cfg.current_enabled && !port_ok) {
        in->current_flags = (uint8_t)(in->current_flags | KILN_CURF_CT_FAULT);
    }
    in->current_deviation       = app->cur_deviation;
    in->current_deviation_valid = app->cur_deviation_valid;
    in->heat_enable_asserted    = app->heat_authorised;
    in->contactor_ops           = app->counters.contactor_ops;
    for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
        in->ssr_ops[ch] = app->counters.ssr_ops[ch];
    }
}

void kiln_app_safety_cycle(kiln_app_t *app, float dt_s)
{
    if (!app || !(dt_s >= 0.0f)) return;

    kiln_safety_input_t in;
    build_safety_input(app, &in);

    /* NFR-17: the checked form, so a contract violation here is counted rather
     * than absorbed into a fail-safe verdict nobody looks at. */
    kiln_safety_verdict_t v;
    if (kiln_safety_eval_checked(&app->safety, &in, dt_s, &v) != KILN_OK) {
        app->safety_bad_calls++;
    }

    /* The measurement has now been consumed by the rules that count windows. */
    app->cur_fresh = false;
    if (app->ports.current) kiln_current_mark_stale(&app->cur);

    app->warnings = (app->warnings & (KILN_WARN_BIT(KILN_WARN_HOLDBACK) |
                                      KILN_WARN_BIT(KILN_WARN_GAINS_UNTUNED) |
                                      KILN_WARN_BIT(KILN_WARN_LOG_UNAVAIL) |
                                      KILN_WARN_BIT(KILN_WARN_DISPLAY_UNAVAIL) |
                                      KILN_WARN_BIT(KILN_WARN_TIME_UNSYNCED) |
                                      KILN_WARN_BIT(KILN_WARN_WIFI_DOWN)))
                  | v.warnings;

    const kiln_port_heat_t *h = app->ports.heat;

    /* SR-27: the contactor comes open on request, ahead of any verdict. */
    if (v.drop_contactor && h->drop_contactor) {
        h->drop_contactor(h->ctx);
    }

    if (v.fault != KILN_FAULT_NONE) {
        /* SR-17: latch, and stop.  The run record keeps the cause. */
        if (app->state != KILN_STATE_FAULT) {
            finish_run(app, KILN_END_FAULT, v.fault);
        }
        app->fault           = v.fault;
        app->duty_request    = 0;
        app->heat_authorised = false;
        if (h->force_off) h->force_off(h->ctx);
        if (app->ports.alarm && app->ports.alarm->set) {
            app->ports.alarm->set(app->ports.alarm->ctx, KILN_ALARM_FAULT);
        }
        return;
    }

    /* AD-04: this assignment is the whole authority model, and this is the only
     * place in the firmware that makes it. */
    const bool was_authorised = app->heat_authorised;
    app->heat_authorised = v.heat_permitted && is_heating_state(app->state) &&
                           (app->state != KILN_STATE_RUNNING || app->heat_allowed);

    /* FR-CUR-13: the contactor operates once per authority transition. */
    if (app->heat_authorised != was_authorised) {
        app->counters.contactor_ops++;
        if (app->ports.counters && app->ports.counters->add_contactor_ops) {
            app->ports.counters->add_contactor_ops(app->ports.counters->ctx, 1);
        }
    }

    if (app->heat_authorised) {
        /* AD-05 / SR-02: one edge per cycle into the charge pump.  Stop calling
         * this -- crash, hang, deadline miss -- and the coil de-energises in
         * about a second with no code involved. */
        if (h->enable_refresh) h->enable_refresh(h->ctx);
    } else if (h->force_off) {
        h->force_off(h->ctx);
    }

    /* FR-RUN-06: the completion alarm, for its configured duration. */
    if (app->state == KILN_STATE_COMPLETE && app->complete_pending) {
        app->alarm_timer_s += dt_s;
        if (app->alarm_timer_s >= (float)app->cfg.alarm_duration_s) {
            app->complete_pending = false;
            if (app->ports.alarm && app->ports.alarm->set) {
                app->ports.alarm->set(app->ports.alarm->ctx, KILN_ALARM_OFF);
            }
        }
    }
}

/* --- commands ---------------------------------------------------------- */

static void finish_run(kiln_app_t *app, kiln_run_end_t reason, kiln_fault_t fault)
{
    /* Heat goes off here rather than on the next safety cycle.  The window tick
     * reads heat_authorised at 10 ms and the safety cycle runs at 100 ms, so
     * leaving it to the supervisor would keep the SSR driven for up to a tenth of
     * a second after the program had finished -- inside FR-RUN-04's budget, but
     * there is no reason to spend any of it. */
    app->heat_authorised = false;
    app->duty_request    = 0;
    if (app->ports.heat->force_off) app->ports.heat->force_off(app->ports.heat->ctx);

    app->record.end_reason = (uint8_t)reason;
    app->record.fault      = (uint8_t)fault;
    app->record.duration_s = (uint32_t)app->run_elapsed_s;
    uint8_t bands = 0;
    const uint32_t *bd = kiln_safety_band_duty(&app->safety, &bands);
    if (bd) memcpy(app->record.band_duty_s, bd, sizeof(app->record.band_duty_s));
    app->record.contactor_ops = app->counters.contactor_ops;
    for (uint8_t ch = 0; ch < KILN_HEAT_CHANNELS; ch++) {
        app->record.ssr_ops[ch] = app->counters.ssr_ops[ch];
    }

    if (app->ports.counters && app->ports.counters->flush) {
        (void)app->ports.counters->flush(app->ports.counters->ctx);
    }

    if (reason == KILN_END_COMPLETE) {
        app->state            = KILN_STATE_COMPLETE;
        app->complete_pending = true;
        app->alarm_timer_s    = 0.0f;
        if (app->ports.alarm && app->ports.alarm->set) {
            app->ports.alarm->set(app->ports.alarm->ctx, KILN_ALARM_COMPLETE);
        }
    } else {
        app->state = (reason == KILN_END_FAULT) ? KILN_STATE_FAULT : KILN_STATE_IDLE;
    }
}

kiln_err_t kiln_app_start(kiln_app_t *app, const kiln_program_t *prog)
{
    if (!app || !prog) return KILN_ERR_INVALID_ARG;

    /* FR-RUN-10: not while a fault is latched, nor while autotune is running. */
    if (app->fault != KILN_FAULT_NONE) return KILN_ERR_STATE;
    if (app->state != KILN_STATE_IDLE && app->state != KILN_STATE_COMPLETE) {
        return KILN_ERR_STATE;
    }
    if (app->cfg.case_present && app->case_valid &&
        app->case_c > app->cfg.max_case_temp_c) {
        return KILN_ERR_STATE;
    }

    /* FR-RUN-02: validated before anything else happens. */
    const kiln_prog_validation_t pv = kiln_profile_validate(prog, app->cfg.max_temp_c);
    if (pv.code != KILN_PROG_OK) return KILN_ERR_RANGE;

    /* FR-CUR-12: refuse to start when current monitoring is unavailable, unless
     * it has been explicitly disabled.  Disabling it is a decision the operator
     * can make; silently firing without it is not. */
    if (app->cfg.current_enabled) {
        const bool port_ok = app->ports.current &&
                             (!app->ports.current->present ||
                              app->ports.current->present(app->ports.current->ctx, 0));
        if (!port_ok) return KILN_ERR_STATE;
    }

    /* FR-RUN-02's self-check: the chamber channel has to be answering. */
    if (!app->kiln_valid || app->tc_fault_bits != 0) return KILN_ERR_STATE;

    const kiln_setpoint_cfg_t spc = {
        .holdback_band_c = app->cfg.holdback_band_c,
        .dwell_tol_c     = app->cfg.dwell_tol_c,
        .max_temp_c      = app->cfg.max_temp_c,
    };
    const kiln_err_t e = kiln_setpoint_start(&app->sp, &spc, prog, app->kiln_c);
    if (e != KILN_OK) return e;

    kiln_runstate_record_init(&app->record, app->next_run_id++);
    app->record.program = *prog;
    app->record.gains.kp = app->cfg.kp;
    app->record.gains.ki = app->cfg.ki;
    app->record.gains.kd = app->cfg.kd;
    memcpy(app->record.gain_set_name, app->cfg.gain_set_name,
           sizeof(app->record.gain_set_name));
    app->record.peak_c = app->kiln_c;

    kiln_pid_reset(&app->pid);
    kiln_safety_begin_run(&app->safety, NULL);
    kiln_current_begin_run(&app->cur);

    app->run_elapsed_s = 0.0;
    app->heat_allowed  = kiln_setpoint_heat_allowed(&app->sp);
    app->state         = KILN_STATE_RUNNING;
    return KILN_OK;
}

kiln_err_t kiln_app_pause(kiln_app_t *app)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    if (app->state != KILN_STATE_RUNNING) return KILN_ERR_STATE;

    /* FR-RUN-03: heating off and every program timer frozen.  Freezing is simply
     * not calling kiln_setpoint_tick, which is why the generator holds no clock. */
    app->state           = KILN_STATE_PAUSED;
    app->duty_request    = 0;
    app->heat_authorised = false;
    if (app->ports.heat->force_off) app->ports.heat->force_off(app->ports.heat->ctx);
    return KILN_OK;
}

kiln_err_t kiln_app_resume(kiln_app_t *app)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    if (app->state != KILN_STATE_PAUSED) return KILN_ERR_STATE;

    /* FR-CTL-06: pick the output back up where it was rather than stepping. */
    (void)kiln_pid_bumpless(&app->pid, 0, kiln_setpoint_value(&app->sp), app->kiln_c);
    app->state = KILN_STATE_RUNNING;
    return KILN_OK;
}

kiln_err_t kiln_app_abort(kiln_app_t *app)
{
    if (!app) return KILN_ERR_INVALID_ARG;

    /* FR-RUN-04: from any state, and heat off before anything else. */
    app->duty_request    = 0;
    app->heat_authorised = false;
    if (app->ports.heat->force_off) app->ports.heat->force_off(app->ports.heat->ctx);

    if (app->state == KILN_STATE_AUTOTUNE) kiln_autotune_cancel(&app->tune);
    if (is_heating_state(app->state) || app->state == KILN_STATE_PAUSED) {
        finish_run(app, KILN_END_OPERATOR_ABORT, KILN_FAULT_NONE);
    }
    app->state = KILN_STATE_IDLE;
    return KILN_OK;
}

kiln_err_t kiln_app_ack_segment(kiln_app_t *app)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    return kiln_setpoint_ack(&app->sp);
}

kiln_err_t kiln_app_clear_fault(kiln_app_t *app)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    if (app->fault == KILN_FAULT_NONE) return KILN_OK;

    kiln_safety_input_t in;
    build_safety_input(app, &in);

    /* SR-18: the detector's own thresholds decide, so there is no second
     * implementation to disagree with it. */
    if (!kiln_safety_can_clear(&app->safety.cfg, app->fault, &in)) {
        return KILN_ERR_STATE;
    }

    app->fault = KILN_FAULT_NONE;
    app->state = KILN_STATE_IDLE;
    if (app->ports.alarm && app->ports.alarm->set) {
        app->ports.alarm->set(app->ports.alarm->ctx, KILN_ALARM_OFF);
    }
    return KILN_OK;
}

kiln_err_t kiln_app_manual(kiln_app_t *app, uint16_t duty_permille)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    if (app->fault != KILN_FAULT_NONE) return KILN_ERR_STATE;
    if (app->state != KILN_STATE_IDLE && app->state != KILN_STATE_MANUAL &&
        app->state != KILN_STATE_COMPLETE) {
        return KILN_ERR_STATE;
    }
    if (duty_permille > app->cfg.duty_max_permille) return KILN_ERR_RANGE;

    if (app->state != KILN_STATE_MANUAL) {
        kiln_safety_begin_run(&app->safety, NULL);
        kiln_current_begin_run(&app->cur);
        kiln_runstate_record_init(&app->record, app->next_run_id++);
        app->run_elapsed_s = 0.0;
    }
    app->duty_manual = duty_permille;
    app->state       = KILN_STATE_MANUAL;
    return KILN_OK;
}

kiln_err_t kiln_app_autotune(kiln_app_t *app, float setpoint_c)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    if (app->fault != KILN_FAULT_NONE) return KILN_ERR_STATE;
    if (app->state != KILN_STATE_IDLE && app->state != KILN_STATE_COMPLETE) {
        return KILN_ERR_STATE;
    }

    /* FR-TUN-03's lower bound is the caller's, because only the application
     * knows ambient; the upper bound is SR-23's and belongs to the core. */
    if (app->kiln_valid && setpoint_c < app->kiln_c + 50.0f) return KILN_ERR_RANGE;

    kiln_tune_cfg_t tc;
    kiln_autotune_cfg_defaults(&tc);
    tc.setpoint_c         = setpoint_c;
    tc.max_temp_c         = app->cfg.max_temp_c;
    tc.amplitude_permille = app->cfg.tune_amplitude_permille;
    tc.hysteresis_c       = app->cfg.tune_hysteresis_c;
    tc.peak_threshold_c   = app->cfg.tune_peak_threshold_c;
    tc.timeout_s          = app->cfg.tune_timeout_s;

    const kiln_err_t e = kiln_autotune_start(&app->tune, &tc);
    if (e != KILN_OK && e != KILN_ERR_RANGE) return e;

    kiln_safety_begin_run(&app->safety, NULL);
    kiln_current_begin_run(&app->cur);
    kiln_runstate_record_init(&app->record, app->next_run_id++);
    app->run_elapsed_s = 0.0;
    app->state         = KILN_STATE_AUTOTUNE;
    return e;   /* KILN_ERR_RANGE: running, but clamped below what was asked */
}

kiln_err_t kiln_app_idle(kiln_app_t *app)
{
    if (!app) return KILN_ERR_INVALID_ARG;
    if (app->fault != KILN_FAULT_NONE) return KILN_ERR_STATE;
    if (is_heating_state(app->state) || app->state == KILN_STATE_PAUSED) {
        return KILN_ERR_STATE;
    }
    app->state            = KILN_STATE_IDLE;
    app->complete_pending = false;
    if (app->ports.alarm && app->ports.alarm->set) {
        app->ports.alarm->set(app->ports.alarm->ctx, KILN_ALARM_OFF);
    }
    return KILN_OK;
}

void kiln_app_snapshot(const kiln_app_t *app, kiln_snapshot_t *out)
{
    if (!app || !out) return;

    memset(out, 0, sizeof(*out));
    out->kiln_c         = app->kiln_c;
    out->kiln_c_raw     = app->kiln_raw_c;
    out->case_c         = app->case_c;
    out->cj_c           = app->cj_c;
    out->rate_c_per_h   = app->rate_c_per_h;
    out->setpoint_c     = kiln_setpoint_value(&app->sp);
    out->duty_permille  = app->duty_request;
    out->current_a      = kiln_current_amps(&app->cur);
    out->current_ref_a  = kiln_current_ref(&app->cur);
    out->current_flags  = kiln_current_flags(&app->cur);
    out->tc_fault_bits  = app->tc_fault_bits;
    out->case_fault_bits= app->case_fault_bits;
    out->state          = (uint8_t)app->state;
    out->segment_index  = kiln_setpoint_segment(&app->sp);
    out->segment_count  = app->sp.prog.segment_count;
    out->heat_authorised= app->heat_authorised;
    out->holdback_active= kiln_setpoint_holdback(&app->sp);
    out->duty_saturated = app->pid.saturated;
    out->kiln_valid     = app->kiln_valid;
    out->case_valid     = app->case_valid;
}
