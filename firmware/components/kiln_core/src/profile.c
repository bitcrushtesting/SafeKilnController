/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include "kiln_core/profile.h"

static kiln_prog_validation_t ok_result(void)
{
    kiln_prog_validation_t r = { KILN_PROG_OK, KILN_SEG_NONE };
    return r;
}

static kiln_prog_validation_t fail(kiln_prog_valid_t code, uint8_t seg)
{
    kiln_prog_validation_t r = { code, seg };
    return r;
}

kiln_prog_validation_t kiln_profile_validate(const kiln_program_t *p, float max_temp_c)
{
    if (!p) return fail(KILN_PROG_ERR_NO_SEGMENTS, KILN_SEG_NONE);

    if (p->schema_version != KILN_PROGRAM_SCHEMA_VERSION) {
        return fail(KILN_PROG_ERR_SCHEMA, KILN_SEG_NONE);
    }
    if (p->name[0] == '\0') {
        return fail(KILN_PROG_ERR_NAME_EMPTY, KILN_SEG_NONE);
    }
    if (p->segment_count == 0) {
        return fail(KILN_PROG_ERR_NO_SEGMENTS, KILN_SEG_NONE);
    }
    if (p->segment_count > KILN_MAX_SEGMENTS) {
        return fail(KILN_PROG_ERR_TOO_MANY_SEGMENTS, KILN_SEG_NONE);
    }

    /* SR-23: the compile-time ceiling binds even if a caller passes something
     * larger as the configured maximum. */
    const float limit = kiln_clampf(max_temp_c, 0.0f, KILN_TEMP_CEILING_C);

    for (uint8_t i = 0; i < p->segment_count; i++) {
        const kiln_segment_t *s = &p->segments[i];

        if ((float)s->target_c > KILN_TEMP_CEILING_C) {
            return fail(KILN_PROG_ERR_TARGET_ABOVE_CEILING, i);
        }
        if ((float)s->target_c > limit) {
            return fail(KILN_PROG_ERR_TARGET_ABOVE_MAX, i);
        }
        if (s->rate_c_per_h > KILN_RATE_MAX_C_PER_H) {
            return fail(KILN_PROG_ERR_RATE_RANGE, i);
        }
        if (s->dwell_min > KILN_DWELL_MIN_MAX) {
            return fail(KILN_PROG_ERR_DWELL_RANGE, i);
        }
    }

    /* FR-PRG-05: reject an absurdly long program.  Evaluated from 20 degC, a
     * representative cold start; the exact figure only matters near the limit. */
    if (kiln_profile_duration_s(p, 20.0f) > KILN_PROGRAM_MAX_DURATION_S) {
        return fail(KILN_PROG_ERR_TOO_LONG, KILN_SEG_NONE);
    }

    return ok_result();
}

const char *kiln_profile_validation_str(kiln_prog_valid_t code)
{
    switch (code) {
    case KILN_PROG_OK:                        return "ok";
    case KILN_PROG_ERR_NO_SEGMENTS:           return "program has no segments";
    case KILN_PROG_ERR_TOO_MANY_SEGMENTS:     return "too many segments";
    case KILN_PROG_ERR_TARGET_ABOVE_MAX:      return "target above the configured maximum temperature";
    case KILN_PROG_ERR_TARGET_ABOVE_CEILING:  return "target above the absolute temperature ceiling";
    case KILN_PROG_ERR_RATE_RANGE:            return "ramp rate out of range";
    case KILN_PROG_ERR_DWELL_RANGE:           return "dwell time out of range";
    case KILN_PROG_ERR_NAME_EMPTY:            return "program name is empty";
    case KILN_PROG_ERR_TOO_LONG:              return "program would take longer than 168 hours";
    case KILN_PROG_ERR_SCHEMA:                return "unsupported program schema version";
    }
    return "unknown validation error";
}

uint32_t kiln_profile_duration_s(const kiln_program_t *p, float start_c)
{
    if (!p || p->segment_count == 0) return 0;

    double total = 0.0;
    float  from  = start_c;

    for (uint8_t i = 0; i < p->segment_count && i < KILN_MAX_SEGMENTS; i++) {
        const kiln_segment_t *s = &p->segments[i];
        const float target = (float)s->target_c;
        const float span   = target > from ? target - from : from - target;

        if (s->rate_c_per_h > 0) {
            /* FR-CTL-10: a stated rate governs both heating and cooling ramps. */
            total += (double)span * 3600.0 / (double)s->rate_c_per_h;
        }
        /* rate == 0 means "as fast as the kiln allows": the setpoint steps, so
         * the ramp contributes no scheduled time. */

        total += (double)s->dwell_min * 60.0;
        from = target;

        if (total > (double)KILN_PROGRAM_MAX_DURATION_S * 4.0) break;  /* saturate */
    }

    if (total < 0.0) total = 0.0;
    if (total > (double)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)(total + 0.5);
}

float kiln_profile_peak_c(const kiln_program_t *p)
{
    float peak = 0.0f;
    if (!p) return 0.0f;
    for (uint8_t i = 0; i < p->segment_count && i < KILN_MAX_SEGMENTS; i++) {
        if ((float)p->segments[i].target_c > peak) peak = (float)p->segments[i].target_c;
    }
    return peak;
}

void kiln_profile_init_empty(kiln_program_t *p, const char *name)
{
    const kiln_program_t zero = {0};
    *p = zero;
    p->schema_version = KILN_PROGRAM_SCHEMA_VERSION;
    if (name) {
        size_t n = strlen(name);
        if (n >= KILN_PROGRAM_NAME_LEN) n = KILN_PROGRAM_NAME_LEN - 1;
        memcpy(p->name, name, n);
        p->name[n] = '\0';
    }
}

/* --- built-in examples, FR-PRG-09 -------------------------------------- */

typedef struct {
    const char    *name;
    const char    *desc;
    uint8_t        count;
    kiln_segment_t segs[8];
} example_t;

static const example_t k_examples[] = {
    {
        "Bisque cone 06", "Slow bisque firing with a preheat and a hold at top.", 5,
        {
            /* target, rate C/h, dwell min, flags */
            {  100,  60,  30, 0, 0 },   /* drive off water, hold to even out   */
            {  600, 100,   0, 0, 0 },   /* through quartz inversion steadily   */
            {  900, 150,   0, 0, 0 },
            {  999,  80,  20, 0, 0 },   /* cone 06 with a soak                 */
            {   40,  90,   0, 0, 0 },   /* controlled cool                     */
        }
    },
    {
        "Glaze cone 6", "Standard cone 6 glaze firing with a controlled cool.", 5,
        {
            {  150, 100,   0, 0, 0 },
            {  700, 180,   0, 0, 0 },
            { 1100, 130,   0, 0, 0 },
            { 1222,  60,  15, 0, 0 },   /* cone 6 with a short soak            */
            {  800,  85,   0, 0, 0 },   /* slow through the glaze setting band */
        }
    },
    {
        "Glass fuse (full)", "Full fuse for compatible art glass, with an anneal soak.", 6,
        {
            {  540, 220,  20, 0, 0 },   /* bubble squeeze                      */
            {  804,   0,  10, 0, 0 },   /* rate 0: as fast as the kiln allows  */
            {  516, 999,  60, 0, 0 },   /* crash cool, then anneal soak        */
            {  482,  30,   0, 0, 0 },
            {  370,  50,   0, 0, 0 },
            {   40,  80,   0, 0, 0 },
        }
    },
};

uint8_t kiln_profile_example_count(void)
{
    return (uint8_t)(sizeof(k_examples) / sizeof(k_examples[0]));
}

kiln_err_t kiln_profile_example(uint8_t index, kiln_program_t *out)
{
    if (!out) return KILN_ERR_INVALID_ARG;
    if (index >= kiln_profile_example_count()) return KILN_ERR_NOT_FOUND;

    const example_t *e = &k_examples[index];
    kiln_profile_init_empty(out, e->name);

    size_t dn = strlen(e->desc);
    if (dn >= KILN_PROGRAM_DESC_LEN) dn = KILN_PROGRAM_DESC_LEN - 1;
    memcpy(out->description, e->desc, dn);
    out->description[dn] = '\0';

    out->segment_count = e->count;
    out->flags         = KILN_PROG_FLAG_READONLY;
    for (uint8_t i = 0; i < e->count; i++) out->segments[i] = e->segs[i];

    return KILN_OK;
}
