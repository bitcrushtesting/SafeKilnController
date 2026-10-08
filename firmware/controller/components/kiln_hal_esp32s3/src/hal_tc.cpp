/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MAX31856 thermocouple front end (SYS-HW-02, SYS-HW-03, SWR-ACQ-02..SWR-ACQ-10, SWR-SAF-04).
 *
 * Two devices on one SPI bus with separate chip selects, which is SYS-HW-02, and on
 * SPI2 (FSPI) rather than a bus shared with anything that can stall it, which
 * is SYS-HW-03.
 *
 * This adapter decides nothing.  It configures the part, reads six registers in
 * one transaction, and hands up degrees plus the fault bits of SWR-ACQ-10.  The
 * grace period, the latching and every rule that acts on a fault live in
 * kiln_core/safety where a host test can reach them (SWA-01).
 *
 * ---------------------------------------------------------------------------
 * Two things here are load-bearing for safety and are easy to lose
 * ---------------------------------------------------------------------------
 *
 * 1. **Open-circuit detection is disabled out of reset.**  CR0's OCFAULT bits
 *    default to 00, which means the part will *not* report an open couple.
 *    Enabling them is what makes SWR-SAF-04 work at all, and -- because SYS-HW-24 wires
 *    the FAULT pin into the contactor coil -- it is also what makes the
 *    hardware interlock function.  A build that forgot this line would leave a
 *    kiln that cannot detect a disconnected probe and an interlock that never
 *    opens, with nothing visibly wrong.
 *
 * 2. **The fault mask is cleared, not left at its default.**  MASK defaults to
 *    0xFF, which masks every fault off the FAULT pin.  Writing 0x00 is what
 *    connects the part's own detections to SYS-HW-24's interlock.
 *
 * ---------------------------------------------------------------------------
 * What this part cannot tell us
 * ---------------------------------------------------------------------------
 * The MAX31856 does not distinguish a short to VCC from a short to GND; both
 * present as OVUV.  So KILN_TC_FAULT_SHORT_VCC and _SHORT_GND are never set by
 * this adapter, and a shorted couple arrives as fault 3 (reading out of range)
 * rather than fault 2 (short).  The reaction is identical -- SWR-SAF-04 latches
 * either way -- so this costs diagnosis, not safety.  Recorded rather than
 * papered over.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"

#include "kiln_hal/board_pins.h"
#include "kiln_hal/hal_esp32s3.h"

namespace {

const char *TAG = "hal_tc";

} // namespace

/* --- register map ------------------------------------------------------- */

enum : uint8_t {
    REG_CR0   = 0x00,
    REG_CR1   = 0x01,
    REG_MASK  = 0x02,
    REG_CJTH  = 0x0A,   /* the burst below starts here */
    REG_SR    = 0x0F,
    REG_WRITE = 0x80,   /* address | 0x80 selects a write */
};

/* CR0 */
enum : uint8_t {
    CR0_CMODE_AUTO = 0x80,  /* continuous conversion                        */
    CR0_OCFAULT_1  = 0x10,  /* open-circuit detect, < 5 kohm. See note 1.   */
    CR0_FAULTCLR   = 0x02,
    CR0_FILTER_50  = 0x01,  /* 0 = 60 Hz, 1 = 50 Hz (SWR-ACQ-06)             */
};

/* CR1 averaging: 4 samples trades a little latency for a lot of noise, which
 * is the right way round next to a switching multi-kilowatt load (SYS-HW-15). */
enum : uint8_t { CR1_AVG_4 = 0x20 };

/* Fault status register bits.  Unsigned underlying type, unlike the register
 * addresses above: these are OR-ed together into masks, and a uint8_t
 * enumerator promotes to *int* first, which makes the whole mask a signed
 * bitwise operation (bugprone-signed-bitwise).  The register is still 8 bits
 * wide; only the arithmetic changes. */
enum : unsigned {
    SR_CJ_RANGE = 0x80u, SR_TC_RANGE = 0x40u,
    SR_CJ_HIGH  = 0x20u, SR_CJ_LOW   = 0x10u,
    SR_TC_HIGH  = 0x08u, SR_TC_LOW   = 0x04u,
    SR_OVUV     = 0x02u, SR_OPEN     = 0x01u,
};

namespace {

typedef struct {
    spi_device_handle_t dev;
    const char         *name;
    bool                configured;
} tc_dev_t;

/* Two devices, fixed at build time by the board (SYS-HW-02).  A static pair in the
 * adapter, not a global in the core: SWA-03's rule is about kiln_core. */
tc_dev_t  s_dev[2];
bool      s_bus_ready;

/* --- register access ---------------------------------------------------- */

kiln_err_t reg_write(tc_dev_t *d, uint8_t addr, uint8_t value)
{
    uint8_t tx[2] = { (uint8_t)(addr | REG_WRITE), value };
    spi_transaction_t t = {};
    t.length    = 16;
    t.tx_buffer = tx;
    return (spi_device_polling_transmit(d->dev, &t) == ESP_OK) ? KILN_OK : KILN_ERR_IO;
}

/* Burst read of `len` registers from `addr`.  One transaction, because the
 * temperature and the fault status have to describe the same conversion: two
 * transactions could straddle one and report a reading the fault bits disown. */
kiln_err_t reg_read(tc_dev_t *d, uint8_t addr, uint8_t *out, size_t len)
{
    uint8_t tx[8] = { addr };
    uint8_t rx[8] = {};
    if (len + 1u > sizeof(tx)) {
        return KILN_ERR_INVALID_ARG;
    }
    spi_transaction_t t = {};
    t.length    = (len + 1u) * 8u;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(d->dev, &t) != ESP_OK) {
        return KILN_ERR_IO;
    }
    memcpy(out, &rx[1], len);
    return KILN_OK;
}

/* --- port_tc ------------------------------------------------------------ */

kiln_err_t tc_configure(void *ctx, kiln_tc_type_t type, uint8_t line_filter_hz)
{
    tc_dev_t *d = static_cast<tc_dev_t *>(ctx);
    if ((d == nullptr) || (d->dev == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }
    if ((unsigned)type >= (unsigned)KILN_TC_TYPE_COUNT) {
        return KILN_ERR_RANGE;
    }

    /* SWR-ACQ-06.  The part offers 50 or 60 Hz rejection and nothing else, so an
     * unsupported value is a range error rather than a silent default: a kiln
     * rejecting the wrong mains frequency reads noise as temperature. */
    uint8_t cr0 = CR0_CMODE_AUTO | CR0_OCFAULT_1;
    if (line_filter_hz == 50u)      { cr0 |= CR0_FILTER_50; }
    else if (line_filter_hz != 60u) { return KILN_ERR_RANGE; }

    /* MASK first: until it is cleared the FAULT pin stays inert, and SYS-HW-24's
     * interlock hangs off that pin. */
    kiln_err_t e = reg_write(d, REG_MASK, 0x00);
    if (e != KILN_OK) { return e; }

    /* kiln_tc_type_t is ordered B,E,J,K,N,R,S,T, which is the part's own
     * TC_TYPE encoding, so the cast is the mapping.  A static_assert guards the
     * coincidence rather than trusting it. */
    static_assert((int)KILN_TC_TYPE_B == 0 && (int)KILN_TC_TYPE_K == 3 &&
                  (int)KILN_TC_TYPE_T == 7,
                  "kiln_tc_type_t must match the MAX31856 TC_TYPE encoding");
    e = reg_write(d, REG_CR1, (uint8_t)(CR1_AVG_4 | (uint8_t)type));
    if (e != KILN_OK) { return e; }

    e = reg_write(d, REG_CR0, cr0);
    if (e != KILN_OK) { return e; }

    /* Read CR1 back.  An absent or dead part reads 0x00 or 0xFF on a bus with a
     * pull-up, and either would otherwise look like a valid configuration. */
    uint8_t check = 0;
    e = reg_read(d, REG_CR1, &check, 1);
    if (e != KILN_OK) { return e; }
    if (check != (uint8_t)(CR1_AVG_4 | (uint8_t)type)) {
        ESP_LOGE(TAG, "%s: CR1 read back 0x%02x, wrote 0x%02x", d->name, check,
                 (unsigned)(CR1_AVG_4 | (uint8_t)type));
        return KILN_ERR_IO;
    }

    d->configured = true;
    ESP_LOGI(TAG, "%s: type %d, %u Hz rejection, open-circuit detect on",
             d->name, (int)type, (unsigned)(line_filter_hz));
    return KILN_OK;
}

/* sr is taken as unsigned, not uint8_t: a uint8_t argument promotes to int at
 * every one of the masks below. */
uint16_t decode_faults(unsigned sr)
{
    uint16_t bits = 0;
    if ((sr & SR_OPEN) != 0u)                       { bits |= KILN_TC_FAULT_OPEN; }
    if ((sr & SR_OVUV) != 0u)                       { bits |= KILN_TC_FAULT_OVUV; }
    if ((sr & (SR_CJ_RANGE | SR_CJ_HIGH | SR_CJ_LOW)) != 0u) {
        bits |= KILN_TC_FAULT_CJ_RANGE;
    }
    if ((sr & (SR_TC_RANGE | SR_TC_HIGH | SR_TC_LOW)) != 0u) {
        bits |= KILN_TC_FAULT_TC_RANGE;
    }
    return bits;
}

kiln_err_t tc_read(void *ctx, kiln_tc_reading_t *out)
{
    tc_dev_t *d = static_cast<tc_dev_t *>(ctx);
    if ((d == nullptr) || (out == nullptr) || (d->dev == nullptr)) {
        return KILN_ERR_INVALID_ARG;
    }

    /* CJTH, CJTL, LTCBH, LTCBM, LTCBL, SR in one go: six registers, one
     * conversion, one transaction. */
    uint8_t r[6] = {};
    const kiln_err_t e = reg_read(d, REG_CJTH, r, sizeof(r));
    if (e != KILN_OK) {
        out->temp_c     = 0.0f;
        out->cj_c       = 0.0f;
        out->fault_bits = KILN_TC_FAULT_COMMS;
        return e;
    }

    /* All-ones or all-zeros across six registers is a part that is not there.
     * SR alone cannot say so, and SWR-ACQ-10 wants the distinction: this is
     * fault 5 (front end did not answer), not a thermocouple fault. */
    bool all_ff = true, all_00 = true;
    for (size_t i = 0; i < sizeof(r); i++) {
        if (r[i] != 0xFFu) { all_ff = false; }
        if (r[i] != 0x00u) { all_00 = false; }
    }
    if (all_ff || all_00) {
        out->temp_c     = 0.0f;
        out->cj_c       = 0.0f;
        out->fault_bits = KILN_TC_FAULT_COMMS;
        return KILN_ERR_IO;
    }

    /* Cold junction: 14-bit signed, 2^-6 degC per LSB (SWR-ACQ-04).
     *
     * Assembled unsigned and then reinterpreted as signed, rather than shifted
     * as a signed value.  The hot junction below is why this matters rather
     * than being a matter of taste: r[2] << 24 with r[2] >= 0x80 overflows
     * int32_t, which is undefined behaviour, and 0x80 is exactly the case that
     * says "below zero".  Done in uint32_t the shift is defined for every
     * input, and the conversion back is the two's-complement reinterpretation
     * the part's datasheet describes. */
    const uint16_t cj_bits = (uint16_t)(((uint32_t)r[0] << 8u) | (uint32_t)r[1]);
    const int16_t  cj_raw  = (int16_t)cj_bits;
    /* NOLINTNEXTLINE(bugprone-signed-bitwise) -- arithmetic shift, see below */
    out->cj_c = (float)(cj_raw >> 2) / 64.0f;

    /* Linearised hot junction: 19-bit signed, 2^-7 degC per LSB. */
    const uint32_t tc_bits = ((uint32_t)r[2] << 24u) | ((uint32_t)r[3] << 16u) |
                             ((uint32_t)r[4] << 8u);
    const int32_t  tc_raw  = (int32_t)tc_bits;
    /* The shift back down is deliberately signed, which is the one case where
     * bugprone-signed-bitwise has to be overruled rather than satisfied: an
     * arithmetic shift sign-extends, and SWR-SAF-05 depends on a negative reading
     * staying negative, because a reversed couple is detected by the reading
     * *falling*.  A logical shift would read -1 degC as +524 287.
     * NOLINTNEXTLINE(bugprone-signed-bitwise) */
    out->temp_c = (float)(tc_raw >> 13) / 128.0f;

    out->fault_bits = decode_faults(r[5]);
    return KILN_OK;
}

} // namespace

/* --- construction ------------------------------------------------------- */

kiln_err_t kiln_hal_tc_bus_init(void)
{
    if (s_bus_ready) {
        return KILN_OK;
    }
    spi_bus_config_t bus = {};
    bus.mosi_io_num     = KILN_PIN_SPI_MOSI;
    bus.miso_io_num     = KILN_PIN_SPI_MISO;
    bus.sclk_io_num     = KILN_PIN_SPI_SCK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 16;

    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) {
        return KILN_ERR_IO;
    }
    s_bus_ready = true;
    return KILN_OK;
}

kiln_err_t kiln_hal_tc_init(uint8_t which, kiln_port_tc_t *out)
{
    if ((out == nullptr) || (which > 1u)) {
        return KILN_ERR_INVALID_ARG;
    }
    if (!s_bus_ready) {
        const kiln_err_t e = kiln_hal_tc_bus_init();
        if (e != KILN_OK) { return e; }
    }

    tc_dev_t *d = &s_dev[which];
    d->name = (which == 0u) ? "chamber" : "enclosure";

    /* As in hal_current.cpp: ESP-IDF's idiom is zero then assign, and
     * `clock_source` has no zero enumerator, so the zero is briefly invalid.
     * It is named on the next line, before the struct is used.
     * NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) */
    spi_device_interface_config_t dc = {};
    /* Named rather than left at the zero that `= {}` leaves behind: 0 is not a
     * valid soc_periph_spi_clk_src_t, and relying on the driver to treat it as
     * "default" is relying on an undocumented coincidence. */
    dc.clock_source = SPI_CLK_SRC_DEFAULT;
    /* The MAX31856 clocks data out on the falling edge and in on the rising:
     * CPOL 0, CPHA 1, which is mode 1.  Mode 0 reads plausible rubbish. */
    dc.mode            = 1;
    /* 2 MHz, well inside the part's 5 MHz, with margin for the filtering SYS-HW-15
     * puts on these lines. */
    dc.clock_speed_hz  = 2 * 1000 * 1000;
    dc.spics_io_num    = (which == 0u) ? KILN_PIN_TC1_CS : KILN_PIN_TC2_CS;
    dc.queue_size      = 1;
    /* The part needs CS held across the whole multi-byte access, which the
     * driver does per transaction; nothing here spans transactions. */

    if (spi_bus_add_device(SPI2_HOST, &dc, &d->dev) != ESP_OK) {
        return KILN_ERR_IO;
    }

    out->ctx       = d;
    out->configure = tc_configure;
    out->read      = tc_read;
    return KILN_OK;
}
