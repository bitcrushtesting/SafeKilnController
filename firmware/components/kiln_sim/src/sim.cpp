/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "kiln_sim/sim.h"

#define SIM_PI 3.14159265358979323846f

void kiln_sim_cfg_defaults(kiln_sim_cfg_t *cfg)
{
    const kiln_sim_cfg_t d = {
        /* A small studio kiln: full duty settles somewhere above its working
         * range, with a time constant of about forty minutes and the couple of
         * minutes of transport lag that element-to-thermocouple coupling gives. */
        .gain_c           = 1500.0f,
        .tau_s            = 2400.0f,
        .dead_time_s      = 90.0f,
        .ambient_c        = 20.0f,
        .loss_frac        = 0.45f,
        .noise_c          = 0.4f,
        .seed             = 1u,

        .case_ambient_c   = 22.0f,
        .case_coupling    = 0.030f,

        .nominal_a        = 30.0f,
        .element_tc_per_c = 0.00035f,
        .leakage_a        = 0.01f,
        .element_groups   = 3u,
        .mains_hz         = 50.0f,

        .ct_a_per_v       = 120.0f,
        .adc_v_per_count  = 3.3f / 4095.0f,
        .bias_counts      = 2048u,
        .noise_counts     = 2.0f,

        .enable_decay_s   = 1.0f,
    };
    *cfg = d;
}

/* xorshift32: deterministic, seeded, and adequate for sensor noise (TR-12). */
static uint32_t rng_next(kiln_sim_t *s)
{
    uint32_t x = (s->rng != 0u) ? s->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return x;
}

/* Uniform in [-1, 1]. */
static float rng_sym(kiln_sim_t *s)
{
    return (float)((double)rng_next(s) / 2147483647.5 - 1.0);
}

void kiln_sim_init(kiln_sim_t *s, const kiln_sim_cfg_t *cfg)
{
    if ((s == nullptr) || (cfg == nullptr)) {
        return;
    }

    memset(s, 0, sizeof(*s));
    s->cfg    = *cfg;
    s->rng = (cfg->seed != 0u) ? cfg->seed : 1u;
    s->kiln_c = cfg->ambient_c;
    s->case_c = cfg->case_ambient_c;
    s->reported_c = cfg->ambient_c;
    s->burst_rate_hz = 4000u;

    for (uint16_t i = 0; i < KILN_SIM_DEAD_SLOTS; i++) {
        s->dead[i] = 0.0f;
    }
}

void kiln_sim_inject(kiln_sim_t *s, uint32_t faults)
{
    if (s == nullptr) {
        return;
    }

    /* A weld is a physical state, not a reading: the contactor is already shut
     * and stays shut however it is commanded from here on. */
    if ((faults & KILN_INJ_CONTACTOR_WELD) != 0u) {
        s->contactor_closed = true;
    }
    if ((faults & KILN_INJ_TC_STUCK) != 0u) {
        s->stuck_at_c = s->reported_c;
    }
    s->inject |= faults;
}

void kiln_sim_clear(kiln_sim_t *s, uint32_t faults)
{
    if (s == nullptr) {
        return;
    }
    s->inject &= ~faults;
}

void kiln_sim_set_temperature(kiln_sim_t *s, float kiln_c)
{
    if (s == nullptr) {
        return;
    }
    s->kiln_c     = kiln_c;
    s->reported_c = kiln_c;
    for (uint16_t i = 0; i < KILN_SIM_DEAD_SLOTS; i++) {
        s->dead[i] = 0.0f;
    }
}

/* --- electrical model --------------------------------------------------- */

/* The fraction of the elements still drawing current. */
static float element_fraction(const kiln_sim_t *s)
{
    if ((s->inject & KILN_INJ_ELEMENT_OPEN) != 0u) {
        return 0.0f;
    }
    if ((s->inject & KILN_INJ_ELEMENT_PARTIAL) != 0u) {
        const uint8_t g = (s->cfg.element_groups != 0u) ? s->cfg.element_groups : 1u;
        /* One group of g has gone: a step change of a known fraction, which is
         * precisely what SR-28 is written to recognise. */
        return (float)(g - 1u) / (float)g;
    }
    return 1.0f;
}

/* Is current actually flowing right now?  Three things in series, which is the
 * same series the safety argument of architecture section 8.1 rests on. */
static bool conducting(const kiln_sim_t *s)
{
    bool ssr = false;
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        if (s->ssr_on[c]) {
            ssr = true;
        }
    }
    if ((s->inject & KILN_INJ_SSR_SHORTED) != 0u) {
        ssr = true;
    }
    if ((s->inject & KILN_INJ_RELAY_FAIL_ON) != 0u) {
        ssr = true;
    }
    if ((s->inject & KILN_INJ_RELAY_FAIL_OFF) != 0u) {
        ssr = false;
    }

    return ssr && s->contactor_closed;
}

static float current_now(const kiln_sim_t *s)
{
    if (!conducting(s)) {
        return s->cfg.leakage_a;
    }

    /* Elements gain resistance as they heat, so current falls -- the effect SR-28
     * has to correct for before it can call a deviation a lost element group. */
    const float r = 1.0f + s->cfg.element_tc_per_c * (s->kiln_c - s->cfg.ambient_c);
    float a = s->cfg.nominal_a * element_fraction(s) / (r > 0.1f ? r : 0.1f);

    if ((s->inject & KILN_INJ_OVERCURRENT) != 0u) {
        a = s->cfg.nominal_a * 1.6f;
    }
    return a;
}

/* The electrical power actually reaching the elements, as a fraction of full.
 * Current and heat come from the same place, so an injected element failure
 * cannot show up in one channel and not the other. */
static float heat_fraction(const kiln_sim_t *s)
{
    if (!s->contactor_closed) {
        return 0.0f;
    }
    if ((s->inject & KILN_INJ_RELAY_FAIL_OFF) != 0u) {
        return 0.0f;
    }

    float duty = 0.0f;
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        const float d = (float)s->duty_permille[c] / (float)KILN_DUTY_MAX;
        if (d > duty) {
            duty = d;
        }
    }
    if (static_cast<unsigned int>(
            ((s->inject & (KILN_INJ_SSR_SHORTED | KILN_INJ_RELAY_FAIL_ON)) != 0u) &&
            (duty < 1.0f)) != 0u) {
        duty = 1.0f; /* stuck on: full power regardless */
    }

    return duty * element_fraction(s);
}

/* --- plant ------------------------------------------------------------- */

void kiln_sim_step(kiln_sim_t *s, float dt_s)
{
    if ((s == nullptr) || !(dt_s > 0.0f)) {
        return;
    }

    s->t_s += (double)dt_s;

    /* Dead time: push the present drive into a ring and read out what was
     * commanded dead_time_s ago (ASM-01's transport lag). */
    const float u_now = heat_fraction(s);
    const float slot_s = s->cfg.dead_time_s > 0.0f
                       ? s->cfg.dead_time_s / (float)KILN_SIM_DEAD_SLOTS
                       : 0.0f;
    float u_delayed = u_now;
    if (slot_s > 0.0f) {
        s->dead_accum_s += dt_s;
        while (s->dead_accum_s >= slot_s) {
            s->dead_accum_s -= slot_s;
            s->dead[s->dead_head] = u_now;
            s->dead_head = (uint16_t)((s->dead_head + 1u) % KILN_SIM_DEAD_SLOTS);
        }
        u_delayed = s->dead[s->dead_head];   /* the oldest slot */
    }

    /* Effective gain falls with temperature: radiative loss goes as T^4. */
    const float frac = s->kiln_c / 1300.0f;
    const float f4   = frac * frac * frac * frac;
    float gain = s->cfg.gain_c * (1.0f - s->cfg.loss_frac * (f4 > 1.0f ? 1.0f : f4));
    if (gain < 0.0f) {
        gain = 0.0f;
    }

    /* An open lid loses heat fast and gains little. */
    float tau = s->cfg.tau_s;
    if ((s->inject & KILN_INJ_LID_OPEN) != 0u) {
        gain *= 0.15f;
        tau  *= 0.25f;
    }
    if (tau < 1.0f) {
        tau = 1.0f;
    }

    const float drive = gain * u_delayed + s->cfg.ambient_c;
    s->kiln_c += dt_s / tau * (drive - s->kiln_c);
    if (s->kiln_c < -50.0f) {
        s->kiln_c = -50.0f;
    }

    /* The enclosure sees a fraction of the chamber's rise above ambient, with a
     * much longer time constant. */
    float case_target = s->cfg.case_ambient_c +
                        s->cfg.case_coupling * (s->kiln_c - s->cfg.ambient_c);
    if ((s->inject & KILN_INJ_CASE_HEATING) != 0u) {
        case_target += 60.0f;
    }
    s->case_c += dt_s / (tau * 4.0f) * (case_target - s->case_c);

    /* What the sensor reports, after the injected sensor faults. */
    float reported = s->kiln_c + s->cfg.noise_c * rng_sym(s);
    if ((s->inject & KILN_INJ_TC_DRIFT) != 0u) {
        s->drift_c += 0.002f * dt_s;     /* ~7 degC per hour */
        reported   += s->drift_c;
    }
    if ((s->inject & KILN_INJ_TC_REVERSED) != 0u) {
        /* A reversed couple reads the cold junction minus the hot junction rise. */
        reported = s->cfg.ambient_c - (s->kiln_c - s->cfg.ambient_c);
    }
    if ((s->inject & KILN_INJ_TC_STUCK) != 0u) {
        reported = s->stuck_at_c;
    }
    s->reported_c = reported;

    s->current_a = current_now(s);

    /* AD-05: the charge pump decays unless it is being refreshed.  Nothing
     * calling enable_refresh -- a crashed, hung or deadlocked safety task -- and
     * the coil drops with no code involved, which is the whole point of the
     * circuit. */
    s->enable_age_s += dt_s;
    if (s->cfg.enable_decay_s > 0.0f && s->enable_age_s > s->cfg.enable_decay_s) {
        s->heat_enable = false;
    }

    /* The contactor follows the enable line, unless it has been welded shut. */
    if ((s->inject & KILN_INJ_CONTACTOR_WELD) == 0u) {
        s->contactor_closed = s->heat_enable;
    }

    /* Deliver a burst once its samples have had time to be taken. */
    if (s->burst_armed && s->t_s >= s->burst_due_s) {
        const float    i_rms = s->current_a;
        const float    amps_per_count = s->cfg.adc_v_per_count * s->cfg.ct_a_per_v;
        const float    peak_counts = (amps_per_count > 0.0f)
                                   ? i_rms * 1.41421356f / amps_per_count : 0.0f;
        const bool     ct_gone = (s->inject & KILN_INJ_CT_DISCONNECTED) != 0;

        for (uint16_t i = 0; i < s->burst_n && i < KILN_CUR_BURST_MAX; i++) {
            float v;
            if (ct_gone) {
                /* FR-CUR-11, and tasklist A6: an open input has no DC path, so it
                 * sits away from the bias the conditioning establishes, and it
                 * carries no AC at all -- not even the noise floor a live winding
                 * always contributes.  Both symptoms, because the firmware tests
                 * for both. */
                v = 300.0f;
            } else {
                const float phase = 2.0f * SIM_PI * s->cfg.mains_hz *
                                    ((float)i / (float)s->burst_rate_hz);
                v = (float)s->cfg.bias_counts + peak_counts * sinf(phase)
                  + s->cfg.noise_counts * rng_sym(s);
            }
            if (v < 0.0f) {
                v = 0.0f;
            }
            if (v > 4095.0f) {
                v = 4095.0f;
            }
            s->burst_buf[i] = (uint16_t)(v + 0.5f);
        }
        s->burst_armed = false;
        s->burst_due_s = 0.0;
    }
}

/* --- port_tc ----------------------------------------------------------- */

static kiln_err_t sim_tc_configure(void *ctx, kiln_tc_type_t type, uint8_t filter_hz)
{
    (void)ctx; (void)type; (void)filter_hz;
    return KILN_OK;
}

static uint16_t sim_tc_faults(const kiln_sim_t *s)
{
    uint16_t bits = 0;
    if ((s->inject & KILN_INJ_TC_OPEN) != 0u) {
        bits |= KILN_TC_FAULT_OPEN;
    }
    if ((s->inject & KILN_INJ_TC_SHORT) != 0u) {
        bits |= KILN_TC_FAULT_SHORT_GND;
    }
    if ((s->inject & KILN_INJ_TC_RANGE) != 0u) {
        bits |= KILN_TC_FAULT_TC_RANGE;
    }
    if ((s->inject & KILN_INJ_TC_COMMS) != 0u) {
        bits |= KILN_TC_FAULT_COMMS;
    }
    return bits;
}

static kiln_err_t sim_tc_read(void *ctx, kiln_tc_reading_t *out)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    out->fault_bits = sim_tc_faults(s);
    if ((out->fault_bits & KILN_TC_FAULT_COMMS) != 0u) {
        return KILN_ERR_IO;
    }

    out->temp_c = s->reported_c;
    out->cj_c   = s->case_c;
    return KILN_OK;
}

static kiln_err_t sim_case_read(void *ctx, kiln_tc_reading_t *out)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    out->fault_bits = 0;
    out->temp_c     = s->case_c;
    out->cj_c       = s->case_c;
    return KILN_OK;
}

/* --- port_heat --------------------------------------------------------- */

static uint8_t sim_heat_channels(void *ctx) { (void)ctx; return KILN_HEAT_CHANNELS; }

static void sim_heat_set_duty(void *ctx, uint8_t channel, uint16_t permille)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel >= KILN_HEAT_CHANNELS) {
        return;
    }

    const uint16_t d = permille > KILN_DUTY_MAX ? KILN_DUTY_MAX : permille;
    s->duty_permille[channel] = s->forced_off ? 0u : d;
}

static void sim_heat_enable_refresh(void *ctx)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if (s == nullptr) {
        return;
    }
    /* Each refresh re-arms the pump; see enable_decay_s for what is and is not
     * being modelled here. */
    s->heat_enable  = true;
    s->enable_age_s = 0.0f;
    s->forced_off   = false;
}

static void sim_heat_drop_contactor(void *ctx)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if (s == nullptr) {
        return;
    }
    s->heat_enable = false;
    /* A welded contactor does not open, which is what SR-27 exists to find out. */
    if ((s->inject & KILN_INJ_CONTACTOR_WELD) == 0u) {
        s->contactor_closed = false;
    }
}

static void sim_heat_force_off(void *ctx)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if (s == nullptr) {
        return;
    }

    s->forced_off = true;
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        s->duty_permille[c] = 0;
        s->ssr_on[c]        = false;
    }
    sim_heat_drop_contactor(ctx);
}

static bool sim_heat_is_off(void *ctx)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if (s == nullptr) {
        return true;
    }
    for (uint8_t c = 0; c < KILN_HEAT_CHANNELS; c++) {
        if (s->ssr_on[c] || s->duty_permille[c] != 0) {
            return false;
        }
    }
    return true;
}

static void sim_heat_set_level(void *ctx, uint8_t channel, bool on)
{
    kiln_sim_set_ssr((kiln_sim_t *)ctx, channel, on);
}

static uint32_t sim_heat_switch_count(void *ctx, uint8_t channel)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel >= KILN_HEAT_CHANNELS) {
        return 0;
    }
    return s->switch_count[channel];
}

/* The SSR pin level, published by whoever owns the window tick. */
void kiln_sim_set_ssr(kiln_sim_t *s, uint8_t channel, bool on)
{
    if ((s == nullptr) || channel >= KILN_HEAT_CHANNELS) {
        return;
    }
    if (on && !s->ssr_on[channel]) {
        s->switch_count[channel]++;
        s->counters.ssr_ops[channel]++;
    }
    s->ssr_on[channel] = on && !s->forced_off;
}

/* --- port_current ------------------------------------------------------ */

static uint8_t sim_cur_channels(void *ctx) { (void)ctx; return 1u; }

static kiln_err_t sim_cur_configure(void *ctx, uint8_t channel, uint32_t rate_hz)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel != 0) {
        return KILN_ERR_INVALID_ARG;
    }
    if (rate_hz < KILN_CUR_RATE_MIN_HZ) {
        return KILN_ERR_RANGE;
    }
    s->burst_rate_hz = rate_hz;
    return KILN_OK;
}

static kiln_err_t sim_cur_start(void *ctx, uint8_t channel,
                                kiln_cur_window_t window, uint16_t n)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel != 0 || n == 0) {
        return KILN_ERR_INVALID_ARG;
    }
    if (s->burst_armed) {
        return KILN_ERR_BUSY;
    }

    s->burst_armed  = true;
    s->burst_window = window;
    s->burst_n      = n > KILN_CUR_BURST_MAX ? KILN_CUR_BURST_MAX : n;
    s->burst_due_s  = s->t_s + (double)s->burst_n / (double)s->burst_rate_hz;
    return KILN_OK;
}

static kiln_err_t sim_cur_read(void *ctx, uint8_t channel, kiln_cur_burst_t *out)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel != 0 || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (s->burst_armed) {
        return KILN_ERR_BUSY; /* still being taken */
    }
    if (s->burst_n == 0) {
        return KILN_ERR_NOT_FOUND;
    }

    out->samples        = s->burst_buf;
    out->count          = s->burst_n;
    out->sample_rate_hz = s->burst_rate_hz;
    out->t_start_us     = (uint64_t)(s->t_s * 1e6);
    out->window         = s->burst_window;
    out->truncated      = false;

    s->burst_n = 0;     /* consumed */
    return KILN_OK;
}

static void sim_cur_abort(void *ctx, uint8_t channel)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel != 0) {
        return;
    }
    s->burst_armed = false;
    s->burst_n     = 0;
}

static bool sim_cur_present(void *ctx, uint8_t channel)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || channel != 0) {
        return false;
    }
    return (s->inject & KILN_INJ_CT_DISCONNECTED) == 0u;
}

/* --- port_counters ----------------------------------------------------- */

static kiln_err_t sim_ctr_load(void *ctx, kiln_switch_counters_t *out)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    *out = s->counters;
    return KILN_OK;
}

static void sim_ctr_add_contactor(void *ctx, uint32_t n)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if (s != nullptr) {
        s->counters.contactor_ops += n;
    }
}

static void sim_ctr_add_ssr(void *ctx, uint8_t channel, uint32_t n)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s != nullptr) && channel < KILN_HEAT_CHANNELS) {
        s->counters.ssr_ops[channel] += n;
    }
}

static kiln_err_t sim_ctr_flush(void *ctx) { (void)ctx; return KILN_OK; }

static kiln_err_t sim_ctr_reset(void *ctx, const kiln_switch_counters_t *to)
{
    kiln_sim_t *s = (kiln_sim_t *)ctx;
    if ((s == nullptr) || (to == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    s->counters = *to;
    return KILN_OK;
}

/* --- RAM file store ---------------------------------------------------- */

void kiln_sim_fs_init(kiln_sim_fs_t *fs)
{
    if (fs != nullptr) {
        memset(fs, 0, sizeof(*fs));
    }
}

static int sfs_find(kiln_sim_fs_t *fs, const char *path)
{
    for (int i = 0; i < KILN_SIM_FS_FILES; i++) {
        if (fs->files[i].used && strcmp(fs->files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static kiln_err_t sfs_read(void *ctx, const char *path, void *out,
                           size_t cap, size_t *len)
{
    kiln_sim_fs_t *fs = (kiln_sim_fs_t *)ctx;
    if ((fs == nullptr) || (path == nullptr) || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const int i = sfs_find(fs, path);
    if (i < 0) {
        return KILN_ERR_NOT_FOUND;
    }
    if (fs->files[i].len > cap) {
        return KILN_ERR_NO_SPACE;
    }

    memcpy(out, fs->files[i].data, fs->files[i].len);
    if (len != nullptr) {
        *len = fs->files[i].len;
    }
    return KILN_OK;
}

static kiln_err_t sfs_write(void *ctx, const char *path, const void *data, size_t len)
{
    kiln_sim_fs_t *fs = (kiln_sim_fs_t *)ctx;
    if ((fs == nullptr) || (path == nullptr) || (data == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (len > KILN_SIM_FS_FILE_MAX) {
        return KILN_ERR_NO_SPACE;
    }
    if (strlen(path) >= KILN_PATH_MAX) {
        return KILN_ERR_INVALID_ARG;
    }

    int i = sfs_find(fs, path);
    if (i < 0) {
        for (int k = 0; k < KILN_SIM_FS_FILES; k++) {
            if (!fs->files[k].used) { i = k; break; }
        }
        if (i < 0) {
            return KILN_ERR_NO_SPACE;
        }
        fs->files[i].used = true;
        snprintf(fs->files[i].path, sizeof(fs->files[i].path), "%s", path);
    }
    memcpy(fs->files[i].data, data, len);
    fs->files[i].len = (uint16_t)len;
    return KILN_OK;
}

static kiln_err_t sfs_remove(void *ctx, const char *path)
{
    kiln_sim_fs_t *fs = (kiln_sim_fs_t *)ctx;
    if ((fs == nullptr) || (path == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const int i = sfs_find(fs, path);
    if (i < 0) {
        return KILN_ERR_NOT_FOUND;
    }
    memset(&fs->files[i], 0, sizeof(fs->files[i]));
    return KILN_OK;
}

static kiln_err_t sfs_exists(void *ctx, const char *path)
{
    kiln_sim_fs_t *fs = (kiln_sim_fs_t *)ctx;
    if ((fs == nullptr) || (path == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    return sfs_find(fs, path) >= 0 ? KILN_OK : KILN_ERR_NOT_FOUND;
}

static kiln_err_t sfs_list(void *ctx, const char *dir,
                           bool (*fn)(void *user, const char *name, size_t size),
                           void *user)
{
    kiln_sim_fs_t *fs = (kiln_sim_fs_t *)ctx;
    if ((fs == nullptr) || (dir == nullptr) || (fn == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    const size_t dlen = strlen(dir);
    for (int i = 0; i < KILN_SIM_FS_FILES; i++) {
        if (!fs->files[i].used) {
            continue;
        }
        if (strncmp(fs->files[i].path, dir, dlen) != 0) {
            continue;
        }
        if (!fn(user, fs->files[i].path, fs->files[i].len)) {
            break;
        }
    }
    return KILN_OK;
}

static kiln_err_t sfs_usage(void *ctx, size_t *total, size_t *used)
{
    kiln_sim_fs_t *fs = (kiln_sim_fs_t *)ctx;
    if (fs == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    size_t u = 0;
    for (int i = 0; i < KILN_SIM_FS_FILES; i++) {
        if (fs->files[i].used) {
            u += fs->files[i].len;
        }
    }
    if (total != nullptr) {
        *total = (size_t)KILN_SIM_FS_FILES * KILN_SIM_FS_FILE_MAX;
    }
    if (used != nullptr) {
        *used = u;
    }
    return KILN_OK;
}

void kiln_sim_fs_bind(kiln_sim_fs_t *fs, kiln_port_filestore_t *out)
{
    if ((fs == nullptr) || (out == nullptr)) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->ctx          = fs;
    out->read         = sfs_read;
    out->write_atomic = sfs_write;
    out->remove       = sfs_remove;
    out->exists       = sfs_exists;
    out->list         = sfs_list;
    out->usage        = sfs_usage;
}

/* --- binding ----------------------------------------------------------- */

void kiln_sim_bind(kiln_sim_t *s, kiln_sim_ports_t *out)
{
    if ((s == nullptr) || (out == nullptr)) {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->tc.ctx       = s;
    out->tc.configure = sim_tc_configure;
    out->tc.read      = sim_tc_read;

    out->case_tc.ctx       = s;
    out->case_tc.configure = sim_tc_configure;
    out->case_tc.read      = sim_case_read;

    out->heat.ctx             = s;
    out->heat.channel_count   = sim_heat_channels;
    out->heat.set_duty        = sim_heat_set_duty;
    out->heat.set_level       = sim_heat_set_level;
    out->heat.enable_refresh  = sim_heat_enable_refresh;
    out->heat.drop_contactor  = sim_heat_drop_contactor;
    out->heat.force_off       = sim_heat_force_off;
    out->heat.is_off          = sim_heat_is_off;
    out->heat.switch_count    = sim_heat_switch_count;

    out->current.ctx           = s;
    out->current.channel_count = sim_cur_channels;
    out->current.configure     = sim_cur_configure;
    out->current.start_burst   = sim_cur_start;
    out->current.read_burst    = sim_cur_read;
    out->current.abort_burst   = sim_cur_abort;
    out->current.present       = sim_cur_present;

    out->counters.ctx               = s;
    out->counters.load              = sim_ctr_load;
    out->counters.add_contactor_ops = sim_ctr_add_contactor;
    out->counters.add_ssr_ops       = sim_ctr_add_ssr;
    out->counters.flush             = sim_ctr_flush;
    out->counters.reset             = sim_ctr_reset;
}
