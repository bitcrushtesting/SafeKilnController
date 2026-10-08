/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's composition root: bring up the peripherals, then run one
 * loop at 10 Hz that reads, decides and reports.
 *
 * Everything that *decides* is in sup_core and is tested on the host.  What is
 * here is the part that needs the silicon, which is the same split SWA-01 makes
 * on the other side of the link and for the same reason.
 *
 * ---------------------------------------------------------------------------
 * STATE: the peripheral bring-up below is NOT yet verified
 * ---------------------------------------------------------------------------
 * The trip logic, the wire format and the MAX31856 register decode are complete
 * and tested on the host; the decode lives in sup_core for that reason, and this
 * file holds only the transaction that feeds it.
 *
 * What remains unverified is the transport: the register sequences here are
 * written against ST's RM0444 and the MAX31856 datasheet but have never run on
 * silicon, and the alternate-function numbers in pins.h are unconfirmed (see the
 * note there).  They are deliberately kept as small, separate functions so each
 * can be brought up and checked one at a time.  Tasklist R6 and R8 cover that
 * work.
 *
 * Every spin in this file is bounded.  A supervisor that can be stalled by a
 * wedged peripheral has put its peripherals ahead of its job, and the two that
 * could stall it are the SPI exchange on the path that decides whether heat is
 * permitted, and the UART on the path that only reports.  Both give up rather
 * than wait; the SPI timeout yields a burst of zeros, which the decode already
 * reports as a comms fault, and an abandoned frame reads to the ESP32 as
 * silence, which it already withholds heat on.
 */
#include <stdint.h>

#include "cortex_m0plus.h"
#include "pins.h"
#include "stm32g031.h"
#include "sup/max31856.h"
#include "sup/trip.h"
#include "sup_proto.h"

namespace {

/* HSI16 is the reset default: 16 MHz, no PLL, no external crystal, and ample
 * for a 10 Hz loop.  Running from the internal oscillator is also one less
 * component whose failure the supervisor would have to survive, and the IWDG
 * has its own oscillator regardless (see board_watchdog_init). */
constexpr uint32_t SUP_SYSCLK_HZ = 16000000u;
constexpr uint32_t SUP_UART_BAUD = 115200u;

/* 10 Hz, matching the report rate the link is specified at and the cycle the
 * ESP32's own supervisor task runs. */
constexpr float    CYCLE_S     = 0.1f;
constexpr uint32_t CYCLE_HZ    = 10u;

/* SysTick reload for one cycle.  The counter is a down-counter that reloads on
 * reaching zero, so the period is RVR + 1 ticks: the reload holds one less than
 * the count.
 *
 * This is the whole of what used to be a calibrated nop loop.  SWR-ACQ-03 gives
 * the cycle period a +/-10 % tolerance, and a spin loop cannot hold one: it
 * drifts with the compiler, the optimisation level and anything added to the
 * loop body, and it is wrong by construction the moment the body does more work
 * on one pass than another.  SysTick is derived from the same clock the
 * tolerance is specified against. */
constexpr uint32_t CYCLE_RELOAD = (SUP_SYSCLK_HZ / CYCLE_HZ) - 1u;
static_assert(CYCLE_RELOAD <= SYST_RVR_MAX,
              "one cycle does not fit in SysTick's 24 bits; a reload that is "
              "truncated is a silently faster cycle, not a slower one");

/* How long tc_init waits for the front end's first conversion.
 *
 * The MAX31856 needs a few hundred milliseconds to produce its first reading
 * with 4-sample averaging and 50 Hz rejection, and until it does the
 * temperature registers read zero.  Zero is in range and carries no fault bit,
 * so it would decode as a perfectly plausible 0 degC and permit heat.  Waiting
 * for a real conversion before sup_init is what stops a boot-time reading of
 * "cold" on a kiln that is not.
 *
 * Generous on purpose: a failed self-test latches SUP_TRIP_SELF_TEST, which
 * sup_clear deliberately refuses to clear, so this must not be a race that a
 * slow part can lose.  20 cycles is 2 s against an expected 0.3 s. */
constexpr uint32_t TC_FIRST_CONVERSION_CYCLES = 20u;

/* --- board, unverified -------------------------------------------------- */

void board_clocks_init()
{
    /* Reset defaults are kept: HSI16, flash latency 0, which is valid at
     * 16 MHz.  Stated rather than written, so a later change to the clock has
     * to come back here and reconsider the latency. */
    RCC_IOPENR  |= SUP_RCC_IOPENR_PORTA;
    RCC_APBENR2 |= SUP_RCC_APBENR2_SPI1;
    RCC_APBENR1 |= SUP_RCC_APBENR1_USART2;
}

void pin_mode(unsigned pin, unsigned mode, unsigned pull)
{
    GPIOA_MODER = (GPIOA_MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
    GPIOA_PUPDR = (GPIOA_PUPDR & ~(3u << (pin * 2u))) | (pull << (pin * 2u));
}

void pin_af(unsigned pin, unsigned af)
{
    if (pin < 8u) {
        GPIOA_AFRL = (GPIOA_AFRL & ~(0xFu << (pin * 4u))) | (af << (pin * 4u));
    } else {
        const unsigned p = pin - 8u;
        GPIOA_AFRH = (GPIOA_AFRH & ~(0xFu << (p * 4u))) | (af << (p * 4u));
    }
}

void board_gpio_init()
{
    /* The permit line was already driven low in reset_handler, before .data
     * was copied.  Re-stating the mode here is harmless and keeps this
     * function a complete description of the pin states. */
    pin_mode(SUP_PIN_PERMIT, 1u, 0u);           /* output, no pull */
    pin_mode(SUP_PIN_TC_CS,  1u, 0u);
    GPIOA_BSRR = (1u << SUP_PIN_TC_CS);         /* ~CS idle high */

    pin_mode(SUP_PIN_TC_FAULT, 0u, 1u);         /* input, pull-up */
    pin_mode(SUP_PIN_CLEAR,    0u, 1u);

    pin_af(SUP_PIN_SPI_SCK,  SUP_AF_SPI1);
    pin_af(SUP_PIN_SPI_MISO, SUP_AF_SPI1);
    pin_af(SUP_PIN_SPI_MOSI, SUP_AF_SPI1);
    pin_mode(SUP_PIN_SPI_SCK,  2u, 0u);         /* alternate function */
    pin_mode(SUP_PIN_SPI_MISO, 2u, 0u);
    pin_mode(SUP_PIN_SPI_MOSI, 2u, 0u);

    pin_af(SUP_PIN_UART_TX, SUP_AF_USART2);
    pin_mode(SUP_PIN_UART_TX, 2u, 0u);

    /* USART2_RX would be PA3, and PA3 is the ~FAULT input instead.  The link
     * is simplex and the receiver is unwired, not merely unused. */
}

void board_uart_init()
{
    /* 8N1, transmit only.  RE is deliberately not set: with no receive pin
     * configured there is nothing to receive, and leaving the receiver off
     * means a noise burst on an adjacent track cannot even fill a register.
     * Oversampling by 16 is the reset default, so BRR is the plain divisor. */
    USART2_CR1 = 0u;                            /* disable while configuring */
    USART2_BRR = SUP_SYSCLK_HZ / SUP_UART_BAUD;
    USART2_CR2 = 0u;
    USART2_CR3 = 0u;
    USART2_CR1 = (1u << 3) | (1u << 0);         /* TE | UE */
}

void board_spi_init()
{
    /* SPI1 as master, mode 1, 2 MHz, 8-bit, chip select driven by software.
     *
     * Bit positions are from RM0444's SPI chapter; they are written inline with
     * their names the same way USART2's are above, because naming two dozen
     * one-use bits in a header would be a header nobody reads against a part
     * nobody has the reference manual open for.
     *
     * MODE 1 (CPOL 0, CPHA 1) is the MAX31856's, and it is the one setting here
     * that is not a free choice: mode 0 reads plausible rubbish rather than
     * failing, which the ESP32 adapter records having found out the hard way.
     *
     * 2 MHz is PCLK / 8, matching the ESP32 adapter's clock exactly so the two
     * front ends are driven identically. The part allows 5 MHz; there is nothing
     * to spend the margin on at 10 Hz.
     *
     * SSM and SSI are both set. In master mode with software slave management
     * off, a low NSS input raises MODF and silently drops the peripheral out of
     * master mode; ~CS is a plain GPIO here (PA4), so the internal NSS has to be
     * held high instead of left to float. */
    constexpr uint32_t CR1_CPHA = (1u << 0);
    constexpr uint32_t CR1_MSTR = (1u << 2);
    constexpr uint32_t CR1_BR_DIV8 = (2u << 3);      /* 16 MHz / 8 = 2 MHz   */
    constexpr uint32_t CR1_SPE  = (1u << 6);
    constexpr uint32_t CR1_SSI  = (1u << 8);
    constexpr uint32_t CR1_SSM  = (1u << 9);
    /* DS = 0b0111 is 8-bit, and FRXTH must agree with it: left at its 16-bit
     * default, RXNE would only rise after two bytes and every single-byte
     * exchange would hang. The two are one setting written as two bits. */
    constexpr uint32_t CR2_DS_8BIT = (7u << 8);
    constexpr uint32_t CR2_FRXTH   = (1u << 12);

    SPI1_CR1 = 0u;                      /* disable while configuring */
    SPI1_CR2 = CR2_DS_8BIT | CR2_FRXTH;
    SPI1_CR1 = CR1_CPHA | CR1_MSTR | CR1_BR_DIV8 | CR1_SSI | CR1_SSM;
    SPI1_CR1 |= CR1_SPE;
}

void board_systick_init()
{
    /* Free-running, no interrupt: the loop polls COUNTFLAG. An interrupt would
     * need a vector table entry and a handler whose only job is to set a flag
     * this function could read directly, and the supervisor has nothing else to
     * do while it waits.
     *
     * CVR is cleared so the first period is a whole one rather than whatever the
     * counter happened to hold out of reset. */
    SYST_RVR = CYCLE_RELOAD;
    SYST_CVR = 0u;
    SYST_CSR = SYST_CSR_CLKSOURCE | SYST_CSR_ENABLE;
    (void)SYST_CSR;                     /* clear a COUNTFLAG set by the above */
}

void board_watchdog_init()
{
    /* The IWDG runs from the LSI, not the system clock, which is the property
     * that made the G0 the choice: a stopped or wrong system clock does not
     * stop the watchdog.  Once started it cannot be disabled in software.
     *
     * The period is bounded from both ends and the bounds nearly touch.
     *
     *   It must be comfortably LONGER than one cycle (100 ms) or the
     *   supervisor resets itself for being busy.
     *
     *   It must be comfortably SHORTER than SWR-NFR-04's 500 ms, because a hung
     *   supervisor stops feeding the watchdog, and the reset is what drops the
     *   permit line (startup.cpp does it before .data is copied).  The
     *   watchdog period is therefore the worst-case time from "the supervisor
     *   stopped working" to "the coil is open", and that has to fit inside the
     *   de-energise budget.
     *
     * LSI is 32 kHz nominal.  PR = 4 divides by 64, giving a 2 ms tick, and
     * RLR = 175 gives 350 ms: three and a half cycles of margin below, and
     * 150 ms of margin above, which holds even if the LSI is 10 % off in
     * either direction.  The 1 s this was first written with would have missed
     * SWR-NFR-04 outright. */
    IWDG_KR  = 0x0000CCCCu;                     /* start                     */
    IWDG_KR  = 0x00005555u;                     /* enable register access    */
    IWDG_PR  = 4u;                              /* LSI / 64 -> 2 ms per tick */
    IWDG_RLR = 175u;                            /* 350 ms                    */
    IWDG_KR  = 0x0000AAAAu;                     /* reload                    */
}

void watchdog_feed() { IWDG_KR = 0x0000AAAAu; }

/* Wait for the end of the current cycle.
 *
 * COUNTFLAG is a one-shot event that is cleared by reading CSR, so this consumes
 * exactly one wrap and returns. The period is therefore the timer's, not the
 * loop body's, and the body's duration does not accumulate into it.
 *
 * An overrun, a body that took longer than one period, is not detected here and
 * is not meant to be: COUNTFLAG says "at least one wrap happened", never how
 * many, so counting missed periods from it is not possible. The IWDG is the
 * overrun detector, at 350 ms against this 100 ms, and its answer to a
 * supervisor that has stopped keeping time is to reset it and drop the permit
 * line, which is the right answer and needs no bookkeeping here. */
void wait_for_cycle_end()
{
    while ((SYST_CSR & SYST_CSR_COUNTFLAG) == 0u) {
    }
}

bool pin_low(unsigned pin) { return (GPIOA_IDR & (1u << pin)) == 0u; }

void permit(bool allow)
{
    GPIOA_BSRR = allow ? (1u << SUP_PIN_PERMIT)
                       : (1u << (SUP_PIN_PERMIT + 16u));
}

/* One byte takes about 139 us at 115200 baud, which is 2 224 cycles at 16 MHz.
 * 50 000 is a factor of twenty above that, so this cannot expire on a working
 * transmitter and cannot be reached by a slow one either: only by a stopped one.
 *
 * The bound exists because a supervisor that can be stalled by its own reporting
 * path has put the report ahead of the job. If the transmitter wedges, the frame
 * is abandoned and the cycle continues; the ESP32 reads an abandoned frame as
 * silence, and withholding heat on silence is already what it does. Reporting is
 * the one thing here allowed to fail. */
constexpr uint32_t UART_TXE_SPINS = 50000u;

void uart_write(const uint8_t *b, uint32_t n)
{
    /* TXE is bit 7 of USART_ISR on this part, from the SVD. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t spins = 0;
        while ((USART2_ISR & (1u << 7)) == 0u) {
            if (++spins >= UART_TXE_SPINS) {
                return;         /* the transmitter is not moving; give up */
            }
        }
        USART2_TDR = b[i];
    }
}

/* --- the thermocouple front end, unverified ----------------------------- */

/* DR has to be reached 8 bits at a time.  With DS set to 8-bit, a 16-bit access
 * pushes two bytes into the transmit FIFO and a 32-bit one is not meaningful at
 * all, so the generated header's 32-bit accessor is the wrong width here.  This
 * is the same cast SUP_REG32 performs, at the width this register needs. */
inline volatile uint8_t *spi1_dr8()
{
    return reinterpret_cast<volatile uint8_t *>(SPI1_BASE + 0xCu);
}

void cs_low()  { GPIOA_BSRR = (1u << (SUP_PIN_TC_CS + 16u)); }
void cs_high() { GPIOA_BSRR = (1u << SUP_PIN_TC_CS); }

/* Exchange one byte.  Full duplex: every byte clocked out clocks one in, which
 * is why a read sends dummy bytes and a write discards what arrives.
 *
 * Bounded for the same reason uart_write is, and more sharply: the thermocouple
 * read is on the path that decides whether heat is permitted, so a wedged SPI
 * bus must not be able to stop the cycle. 2 MHz makes a byte 4 us, 64 cycles;
 * 10 000 is far beyond any working transfer. A timeout returns 0x00, and a burst
 * of 0x00 is exactly what sup_tc_decode reports as a comms fault. */
constexpr uint32_t SPI_SPINS = 10000u;

uint8_t spi_xfer(uint8_t out)
{
    constexpr uint32_t SR_RXNE = (1u << 0);
    constexpr uint32_t SR_TXE  = (1u << 1);

    uint32_t spins = 0;
    while ((SPI1_SR & SR_TXE) == 0u) {
        if (++spins >= SPI_SPINS) { return 0x00u; }
    }
    *spi1_dr8() = out;

    spins = 0;
    while ((SPI1_SR & SR_RXNE) == 0u) {
        if (++spins >= SPI_SPINS) { return 0x00u; }
    }
    return *spi1_dr8();
}

void tc_write_reg(uint8_t reg, uint8_t value)
{
    cs_low();
    (void)spi_xfer((uint8_t)(reg | SUP_TC_REG_WRITE));
    (void)spi_xfer(value);
    cs_high();
}

uint8_t tc_read_reg(uint8_t reg)
{
    cs_low();
    (void)spi_xfer(reg);                /* address, read when bit 7 is clear */
    const uint8_t v = spi_xfer(0x00u);
    cs_high();
    return v;
}

struct tc_reading {
    float    chamber_c;
    float    cj_c;
    uint16_t fault_bits;
    bool     valid;
};

/* One burst from CJTH to SR: six registers, one conversion, one transaction.
 * Split across two transactions the temperature and the fault status could
 * straddle a conversion, and the reading would be disowned by fault bits that
 * describe a different sample. */
void tc_burst(uint8_t out[SUP_TC_BURST_BYTES])
{
    cs_low();
    (void)spi_xfer(SUP_TC_REG_CJTH);
    for (unsigned i = 0; i < SUP_TC_BURST_BYTES; i++) {
        out[i] = spi_xfer(0x00u);
    }
    cs_high();
}

tc_reading tc_read()
{
    uint8_t regs[SUP_TC_BURST_BYTES] = {};
    tc_burst(regs);

    sup_tc_sample_t sample = {};
    sup_tc_decode(regs, &sample);       /* the decode is in sup_core, tested */

    tc_reading r = {};
    r.chamber_c  = sample.chamber_c;
    r.cj_c       = sample.cj_c;
    r.fault_bits = sample.fault_bits;
    r.valid      = sample.valid;
    return r;
}

/* Configure the front end and prove it is there.
 *
 * Returning false is what makes sup_init's self-test argument real: a front end
 * that did not configure must never be followed by a permit. Three things have
 * to hold, and all three are checked rather than assumed:
 *
 *   MASK is written FIRST, because until it is cleared the part's ~FAULT output
 *   stays inert, and PA3 would read "no fault" from a part that had not yet been
 *   told to report any.
 *
 *   CR1 is read back. An absent part, or a bus stuck at either rail, reads 0x00
 *   or 0xFF, and neither is a configured CR1. This is the check that
 *   distinguishes "configured" from "wrote into the void".
 *
 *   A conversion has to actually complete. Before the first one the temperature
 *   registers read zero, which decodes as a plausible 0 degC with no fault bits:
 *   the one reading that would permit heat on a kiln whose temperature is not yet
 *   known. */
bool tc_init()
{
    tc_write_reg(SUP_TC_REG_MASK, SUP_TC_MASK_VALUE);
    tc_write_reg(SUP_TC_REG_CR1,  SUP_TC_CR1_VALUE);
    tc_write_reg(SUP_TC_REG_CR0,  SUP_TC_CR0_VALUE);

    if (!sup_tc_cr1_ok(tc_read_reg(SUP_TC_REG_CR1))) {
        return false;
    }

    for (uint32_t i = 0; i < TC_FIRST_CONVERSION_CYCLES; i++) {
        /* The watchdog is already running and expires at 350 ms, while this
         * loop can run for 2 s. Without this feed the wait for a first
         * conversion would reset the part, every boot, forever. */
        watchdog_feed();
        wait_for_cycle_end();
        const tc_reading r = tc_read();
        /* Either answer ends the wait. A valid reading is the one wanted; a
         * reported fault is also a completed conversion, and it is the trip
         * logic's business rather than the self-test's: SUP_TRIP_SELF_TEST is
         * unclearable, and an open thermocouple on a bench is not a reason to
         * make a board need a power cycle. */
        if (r.valid || (r.fault_bits != (uint16_t)SUP_TC_FAULT_COMMS)) {
            return true;
        }
    }
    return false;       /* nothing ever answered */
}

}  // namespace

int main()
{
    board_clocks_init();
    board_gpio_init();
    board_uart_init();
    board_spi_init();
    /* Before tc_init, which waits whole cycles for the front end's first
     * conversion and therefore needs the timer running. */
    board_systick_init();
    board_watchdog_init();

    /* tc_init can take up to TC_FIRST_CONVERSION_CYCLES, which is 2 s against
     * the watchdog's 350 ms, so the watchdog has to be fed while it waits. The
     * feed is inside the wait loop rather than here. */
    const bool selftest_ok = tc_init();

    sup_t sup;
    sup_init(&sup, selftest_ok);

    uint8_t seq = 0;
    for (;;) {
        watchdog_feed();

        const tc_reading tc = tc_read();

        sup_input_t in = {};
        in.chamber_c     = tc.chamber_c;
        in.chamber_valid = tc.valid && !pin_low(SUP_PIN_TC_FAULT);
        in.fault_bits    = tc.fault_bits;
        /* The button is a level on a pin; whether it *means* anything is
         * sup_step's decision, because it is edge triggered and held and that
         * belongs in the part that has tests. */
        in.clear_pressed = pin_low(SUP_PIN_CLEAR);

        sup_step(&sup, &in, CYCLE_S);

        /* The output is driven every cycle rather than on change, so a bit
         * corrupted in the GPIO register is corrected within one cycle. */
        permit(sup.permit);

        sup_report_t rep = {};
        rep.version     = SUP_VERSION;
        rep.seq         = seq++;
        rep.chamber_c   = tc.chamber_c;
        rep.cj_c        = tc.cj_c;
        rep.fault_bits  = tc.fault_bits;
        rep.flags       = sup_flags(&sup, &in);
        rep.trip_reason = sup.reason;

        uint8_t frame[SUP_FRAME_BYTES];
        if (sup_encode(&rep, frame, sizeof(frame)) == SUP_FRAME_BYTES) {
            uart_write(frame, SUP_FRAME_BYTES);
        }

        wait_for_cycle_end();
    }
}
