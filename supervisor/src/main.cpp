/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's composition root: bring up the peripherals, then run one
 * loop at 10 Hz that reads, decides and reports.
 *
 * Everything that *decides* is in sup_core and is tested on the host.  What is
 * here is the part that needs the silicon, which is the same split AD-01 makes
 * on the other side of the link and for the same reason.
 *
 * ---------------------------------------------------------------------------
 * STATE: the peripheral bring-up below is NOT yet verified
 * ---------------------------------------------------------------------------
 * The trip logic and the wire format are complete and tested.  The register
 * sequences in sup_board_* are written against ST's reference manual but have
 * never run on silicon, and the alternate-function numbers in pins.h are
 * unconfirmed (see the note there).  They are deliberately kept as small,
 * separate functions so each can be brought up and checked one at a time.
 * Tasklist R6 and R8 cover that work.
 */
#include <stdint.h>

#include "pins.h"
#include "stm32g031.h"
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
constexpr float   CYCLE_S     = 0.1f;
/* Four cycles of the spin below per iteration is a guess, which is why R6
 * leaves this as a placeholder and R8 replaces the whole thing with a timer:
 * the cycle period is a timing requirement (FR-ACQ-03, +/-10 %), not an
 * approximation. */
constexpr uint32_t CYCLE_TICKS = SUP_SYSCLK_HZ / 40u;

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
     *   It must be comfortably SHORTER than NFR-04's 500 ms, because a hung
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
     * NFR-04 outright. */
    IWDG_KR  = 0x0000CCCCu;                     /* start                     */
    IWDG_KR  = 0x00005555u;                     /* enable register access    */
    IWDG_PR  = 4u;                              /* LSI / 64 -> 2 ms per tick */
    IWDG_RLR = 175u;                            /* 350 ms                    */
    IWDG_KR  = 0x0000AAAAu;                     /* reload                    */
}

void watchdog_feed() { IWDG_KR = 0x0000AAAAu; }

/* TODO(R6): a timer, not a spin. A spin loop is wrong once the clock is
 * configured, and the cycle period is a timing requirement (FR-ACQ-03's
 * +/-10 %), not an approximation. */
void delay_one_cycle()
{
    /* A nop the compiler may not elide, rather than a volatile counter:
     * incrementing a volatile is deprecated in C++20 and -Werror says so. */
    for (uint32_t i = 0; i < CYCLE_TICKS; i++) {
        __asm__ volatile("nop");
    }
}

bool pin_low(unsigned pin) { return (GPIOA_IDR & (1u << pin)) == 0u; }

void permit(bool allow)
{
    GPIOA_BSRR = allow ? (1u << SUP_PIN_PERMIT)
                       : (1u << (SUP_PIN_PERMIT + 16u));
}

void uart_write(const uint8_t *b, uint32_t n)
{
    /* TXE is bit 7 of USART_ISR on this part, from the SVD.
     *
     * The spin is bounded in practice: 13 bytes at 115200 baud is about 1.1 ms
     * against a 100 ms cycle.  It is still a spin, and R8 should give it a
     * bound, because a supervisor that can be stalled by its own reporting
     * path has put the report ahead of the job. */
    for (uint32_t i = 0; i < n; i++) {
        while ((USART2_ISR & (1u << 7)) == 0u) {
        }
        USART2_TDR = b[i];
    }
}

/* --- the thermocouple front end, unverified ----------------------------- */

struct tc_reading {
    float    chamber_c;
    float    cj_c;
    uint16_t fault_bits;
    bool     valid;
};

bool tc_init()
{
    /* TODO(R6): write CR0/CR1/MASK over SPI, type K fixed (FR-ACQ-02), 50 Hz
     * notch, open-circuit detect on, and read the registers back.  Returning
     * false here is what makes sup_init's self-test argument real: a front end
     * that did not configure must never be followed by a permit. */
    return false;
}

tc_reading tc_read()
{
    /* TODO(R6): burst-read CJTH..SR, decode as the ESP32 adapter does, and
     * set valid only on a conversion that completed in range. */
    tc_reading r = {};
    r.valid = false;
    return r;
}

}  // namespace

int main()
{
    board_clocks_init();
    board_gpio_init();
    board_uart_init();
    board_watchdog_init();

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

        delay_one_cycle();
    }
}
