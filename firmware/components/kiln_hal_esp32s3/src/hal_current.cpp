/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Heater current front end: current transformer into ADC1 via HR-17's
 * conditioning (FR-CUR-01..FR-CUR-05, FR-CUR-11, FR-CUR-14, AD-17).
 *
 * The port hands up **raw ADC counts**, mid-rail biased exactly as they arrive.
 * Removing the bias, computing true RMS over whole mains cycles and scaling to
 * amps is kiln_core/current, where it is tested against a synthetic waveform
 * (TR-13).  In particular the noise floor must survive to the core: FR-CUR-11
 * distinguishes an absent transformer from a genuine zero by the absence of any
 * signal *at all, including noise*, and an adapter that helpfully squelched
 * small values would destroy that distinction.
 *
 * ---------------------------------------------------------------------------
 * Why the DMA continuous driver and not one-shot reads
 * ---------------------------------------------------------------------------
 * FR-CUR-14 says measurement may not delay the control or safety cycles, and
 * FR-CUR-03 wants RMS over a whole number of mains cycles so the result does
 * not depend on sampling phase.  One-shot reads in a loop would either block a
 * task for 40 ms or jitter badly enough to put energy at the wrong frequency.
 * The continuous driver samples into a DMA pool on its own, and the burst
 * becomes a question of which samples to take, not of waiting for them.
 *
 * The stream runs continuously and a burst is carved out of it.  start_burst
 * therefore *flushes* first: the pool may hold samples from the previous
 * commanded window, and AD-17's whole point is that a measurement belongs to
 * one window.  Mixing conduction and leakage samples in one RMS would make
 * SR-25 and SR-26 meaningless, which is the failure the gating exists to stop.
 *
 * ---------------------------------------------------------------------------
 * One channel, and why (HR-23, tasklist I1)
 * ---------------------------------------------------------------------------
 * FR-CUR-15 wants one transformer per phase and the core and application are
 * already channel-generic.  This adapter nevertheless reports **one** channel,
 * because on the ESP32-S3 the only ADC usable alongside WiFi is ADC1
 * (GPIO1..GPIO10) and this board has no free pin on it -- see the constraint
 * written out in board_pins.h.  Reporting a channel the hardware cannot sample
 * would give the safety rules three readings of which two were invented.
 */
#include <string.h>

#include "esp_adc/adc_continuous.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "hal_current";

} // namespace

/* The pool holds a little over one burst, so a burst is never starved by the
 * driver wrapping, and a stale window is one flush away rather than many. */
#define CUR_POOL_BYTES   (KILN_CUR_BURST_MAX * SOC_ADC_DIGI_RESULT_BYTES * 2u)
#define CUR_FRAME_BYTES  (KILN_CUR_BURST_MAX * SOC_ADC_DIGI_RESULT_BYTES)

namespace {

typedef struct {
    adc_continuous_handle_t handle;
    uint32_t          rate_hz;        /* achieved, which is what FR-CUR-03 wants */
    bool              running;

    /* the burst in flight */
    bool              armed;
    kiln_cur_window_t window;
    uint16_t          want;
    uint16_t          have;
    uint16_t          samples[KILN_CUR_BURST_MAX];

    /* FR-CUR-11: a live winding always contributes some noise. */
    bool              ever_sampled;
    uint16_t          last_min, last_max;
} cur_t;

cur_t s_cur;

/* --- helpers ------------------------------------------------------------ */

void drain_pool(cur_t *c)
{
    uint8_t  scratch[256];
    uint32_t got = 0;
    /* Zero timeout: discard whatever is there and return, never wait. */
    while (adc_continuous_read(c->handle, scratch, sizeof(scratch), &got, 0) == ESP_OK &&
           got > 0u) {
        /* discarded on purpose */
    }
}

/* --- port_current ------------------------------------------------------- */

uint8_t cur_channel_count(void *ctx)
{
    (void)ctx;
    return 1u;      /* see the note at the top of this file */
}

kiln_err_t cur_configure(void *ctx, uint8_t channel, uint32_t sample_rate_hz)
{
    cur_t *c = static_cast<cur_t *>(ctx);
    if ((c == nullptr) || channel != 0u) {
        return KILN_ERR_INVALID_ARG;
    }
    if (sample_rate_hz < KILN_CUR_RATE_MIN_HZ) {
        return KILN_ERR_RANGE;       /* FR-CUR-03's floor */
    }

    if (c->running) {
        (void)adc_continuous_stop(c->handle);
        c->running = false;
    }

    /* ESP-IDF's documented idiom is zero the struct, then set what you need,
     * and `conv_mode` has no zero enumerator -- so the zero is momentarily an
     * invalid enum value.  It is assigned below before the struct is used, and
     * there is no way to write this that both satisfies the check and keeps
     * -Wmissing-field-initializers quiet across IDF versions that add fields.
     * NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
    adc_continuous_config_t cfg = {};
    adc_digi_pattern_config_t pat = {};
    pat.atten     = ADC_ATTEN_DB_12;      /* HR-17 clamps to the ADC rails */
    pat.channel   = (adc_channel_t)KILN_HAL_CURR_ADC_CHANNEL;
    pat.unit      = ADC_UNIT_1;
    pat.bit_width = ADC_BITWIDTH_12;

    cfg.pattern_num    = 1;
    cfg.adc_pattern    = &pat;
    cfg.sample_freq_hz = sample_rate_hz;
    cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
    /* `format` is deprecated as of ESP-IDF 6.0: the driver takes the output
     * layout from the SoC, which on the ESP32-S3 is TYPE2.  The decode below
     * still reads type2 explicitly, so a part whose layout differed would be a
     * compile error rather than silently wrong numbers. */

    if (adc_continuous_config(c->handle, &cfg) != ESP_OK) {
        return KILN_ERR_RANGE;       /* the driver refuses rates it cannot clock */
    }
    if (adc_continuous_start(c->handle) != ESP_OK) {
        return KILN_ERR_IO;
    }

    c->rate_hz = sample_rate_hz;
    c->running = true;
    c->armed   = false;
    ESP_LOGI(TAG, "ADC1 ch%d on IO%d at %u Hz", KILN_HAL_CURR_ADC_CHANNEL,
             KILN_PIN_CURR_SENSE, (unsigned)sample_rate_hz);
    return KILN_OK;
}

kiln_err_t cur_start_burst(void *ctx, uint8_t channel,
                                  kiln_cur_window_t window, uint16_t n_samples)
{
    cur_t *c = static_cast<cur_t *>(ctx);
    if ((c == nullptr) || channel != 0u || n_samples == 0u) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!c->running) {
        return KILN_ERR_STATE;
    }
    if (c->armed) {
        return KILN_ERR_BUSY;
    }

    /* AD-17: this burst belongs to *this* commanded window.  Anything already
     * in the pool belongs to the last one. */
    drain_pool(c);

    c->window = window;
    c->want   = (n_samples > KILN_CUR_BURST_MAX) ? KILN_CUR_BURST_MAX : n_samples;
    c->have   = 0;
    c->armed  = true;
    return KILN_OK;
}

kiln_err_t cur_read_burst(void *ctx, uint8_t channel, kiln_cur_burst_t *out)
{
    cur_t *c = static_cast<cur_t *>(ctx);
    if ((c == nullptr) || channel != 0u || (out == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!c->armed) {
        return KILN_ERR_NOT_FOUND;
    }

    /* Take whatever has arrived and return BUSY until the burst is whole.  The
     * caller polls from its own cycle, so this never waits (FR-CUR-14). */
    uint8_t  frame[CUR_FRAME_BYTES];
    uint32_t got = 0;
    while (c->have < c->want &&
           adc_continuous_read(c->handle, frame, sizeof(frame), &got, 0) == ESP_OK &&
           got > 0u) {
        const uint32_t n = got / SOC_ADC_DIGI_RESULT_BYTES;
        for (uint32_t i = 0; i < n && c->have < c->want; i++) {
            /* Copied out rather than read through a pointer aimed into the
             * byte buffer: `frame` is a uint8_t array, so aliasing it as an
             * adc_digi_output_data_t assumes an alignment adc_continuous_read
             * does not promise.  The copy is one word and the compiler folds
             * it away. */
            adc_digi_output_data_t smp;
            memcpy(&smp, &frame[i * SOC_ADC_DIGI_RESULT_BYTES], sizeof(smp));
            c->samples[c->have++] = (uint16_t)smp.type2.data;
        }
    }

    if (c->have < c->want) {
        return KILN_ERR_BUSY;
    }

    uint16_t lo = 0xFFFFu, hi = 0;
    for (uint16_t i = 0; i < c->have; i++) {
        if (c->samples[i] < lo) { lo = c->samples[i]; }
        if (c->samples[i] > hi) { hi = c->samples[i]; }
    }
    c->last_min     = lo;
    c->last_max     = hi;
    c->ever_sampled = true;

    out->samples        = c->samples;
    out->count          = c->have;
    out->sample_rate_hz = c->rate_hz;
    out->t_start_us     = (uint64_t)esp_timer_get_time();
    out->window         = c->window;
    out->truncated      = false;

    c->armed = false;
    return KILN_OK;
}

void cur_abort_burst(void *ctx, uint8_t channel)
{
    cur_t *c = static_cast<cur_t *>(ctx);
    if ((c == nullptr) || channel != 0u) {
        return;
    }
    /* FR-CUR-05: the window closed early, so discard rather than mix. */
    c->armed = false;
    c->have  = 0;
    drain_pool(c);
}

bool cur_present(void *ctx, uint8_t channel)
{
    const cur_t *c = static_cast<const cur_t *>(ctx);
    if ((c == nullptr) || channel != 0u) {
        return false;
    }
    if (!c->ever_sampled) {
        return c->running;      /* nothing measured yet: no opinion but "wired" */
    }
    /* FR-CUR-11, and the adapter's half of it: the front end's own view is
     * whether the DC bias sits where HR-17's conditioning puts it.  An open
     * input has no DC path and drifts to a rail.  The *absence of a noise
     * floor* is the core's test, not this one, because only the core knows
     * what the floor should look like after scaling.
     *
     * A tenth of full scale either side of mid-rail is generous: the point is
     * to catch a rail, not to grade the conditioning. */
    const uint16_t mid = 4095u / 2u;
    const uint16_t tol = 4095u / 10u;
    const uint16_t avg = (uint16_t)(((uint32_t)c->last_min + c->last_max) / 2u);
    return (avg > (uint16_t)(mid - tol)) && (avg < (uint16_t)(mid + tol));
}

} // namespace

/* --- construction ------------------------------------------------------- */

kiln_err_t kiln_hal_current_init(kiln_port_current_t *out)
{
    if (out == nullptr) {
        return KILN_ERR_INVALID_ARG;
    }

    adc_continuous_handle_cfg_t pool = {};
    pool.max_store_buf_size = CUR_POOL_BYTES;
    pool.conv_frame_size    = CUR_FRAME_BYTES;

    if (adc_continuous_new_handle(&pool, &s_cur.handle) != ESP_OK) {
        return KILN_ERR_IO;
    }

    s_cur.rate_hz      = 0;
    s_cur.running      = false;
    s_cur.armed        = false;
    s_cur.ever_sampled = false;

    out->ctx            = &s_cur;
    out->channel_count  = cur_channel_count;
    out->configure      = cur_configure;
    out->start_burst    = cur_start_burst;
    out->read_burst     = cur_read_burst;
    out->abort_burst    = cur_abort_burst;
    out->present        = cur_present;
    return KILN_OK;
}
