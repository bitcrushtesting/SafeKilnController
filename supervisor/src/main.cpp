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

/* 10 Hz, matching the report rate the link is specified at and the cycle the
 * ESP32's own supervisor task runs. */
constexpr float   CYCLE_S     = 0.1f;
constexpr uint32_t CYCLE_TICKS = 1600000u;   /* placeholder; see delay() */

/* --- board, unverified -------------------------------------------------- */

void board_clocks_init()
{
    /* TODO(R6): HSI16 is the reset default and is enough for this firmware at
     * 10 Hz; confirm flash latency is valid at whatever is finally chosen. */
    FLASH_ACR = FLASH_ACR;                      /* reset default retained */
    RCC_IOPENR |= (1u << 0);                    /* GPIOAEN */
    RCC_APBENR2 |= (1u << 12);                  /* SPI1EN  (confirm bit) */
    RCC_APBENR1 |= (1u << 17);                  /* USART2EN (confirm bit) */
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
    pin_mode(SUP_PIN_LID,      0u, 1u);         /* input, pull-up: NC switch */
    pin_mode(SUP_PIN_CLEAR,    0u, 1u);

    pin_af(SUP_PIN_SPI_SCK,  SUP_AF_SPI1);
    pin_af(SUP_PIN_SPI_MISO, SUP_AF_SPI1);
    pin_af(SUP_PIN_SPI_MOSI, SUP_AF_SPI1);
    pin_mode(SUP_PIN_SPI_SCK,  2u, 0u);         /* alternate function */
    pin_mode(SUP_PIN_SPI_MISO, 2u, 0u);
    pin_mode(SUP_PIN_SPI_MOSI, 2u, 0u);

    pin_af(SUP_PIN_UART_TX, SUP_AF_USART2);
    pin_mode(SUP_PIN_UART_TX, 2u, 0u);
}

void board_watchdog_init()
{
    /* The IWDG runs from the LSI, not the system clock, which is the property
     * that made the G0 the choice: a stopped or wrong system clock does not
     * stop the watchdog.  Once started it cannot be disabled in software.
     * TODO(R6): confirm the prescaler and reload for a period comfortably
     * longer than one cycle and far shorter than NFR-04's budget. */
    IWDG_KR  = 0x0000CCCCu;                     /* start */
    IWDG_KR  = 0x00005555u;                     /* enable register access */
    IWDG_PR  = 4u;                              /* /64 (confirm) */
    IWDG_RLR = 500u;                            /* (confirm) */
    IWDG_KR  = 0x0000AAAAu;                     /* reload */
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
    /* TODO(R6): confirm the TXE/TC bit positions in USART_ISR for this part. */
    for (uint32_t i = 0; i < n; i++) {
        while ((USART1_ISR & (1u << 7)) == 0u) {     /* TXE */
        }
        USART1_TDR = b[i];
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
        in.lid_open      = pin_low(SUP_PIN_LID);   /* NC switch: open reads low */

        sup_step(&sup, &in, CYCLE_S);

        /* The output is driven every cycle rather than on change, so a bit
         * corrupted in the GPIO register is corrected within one cycle. */
        permit(sup.permit);

        if (pin_low(SUP_PIN_CLEAR)) {
            sup_clear(&sup);            /* local only; there is no receive path */
        }

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
