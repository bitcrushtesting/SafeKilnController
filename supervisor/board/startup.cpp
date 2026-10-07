/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reset vector and C runtime bring-up for the supervisor.
 *
 * The first thing the reset handler does, before .data is even copied, is put
 * the coil permit output low.  The order matters: between reset and main() the
 * GPIO is an input with the pin pulled by whatever is on the board, and the
 * supervisor's whole contract is that it does not permit heat until it has
 * decided to.
 */
#include <stdint.h>

#include "stm32g031.h"
#include "pins.h"

extern "C" {

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int  main();
void reset_handler();

/* Anything unexpected ends up here.  It does not return, and the independent
 * watchdog is left to expire, which drops the permit line with it: a
 * supervisor that has lost its way must not keep asserting permission. */
[[noreturn]] void default_handler() noexcept
{
    for (;;) {
    }
}

/* The attributes have to match the alias target or -Wmissing-attributes
 * objects, and it is right to object: an alias that claims less than its
 * target is a lie the optimiser is entitled to believe. */
[[noreturn]] void nmi_handler() noexcept
    __attribute__((weak, alias("default_handler")));
[[noreturn]] void hardfault_handler() noexcept
    __attribute__((weak, alias("default_handler")));

__attribute__((section(".isr_vector"), used))
void *const g_vectors[] = {
    (void *)&_estack,
    (void *)reset_handler,
    (void *)nmi_handler,
    (void *)hardfault_handler,
};

void reset_handler()
{
    /* Outputs safe before anything else, including before .data exists.
     * SUP_PIN_PERMIT low means the series element is open. */
    RCC_IOPENR |= SUP_RCC_IOPENR_PERMIT_PORT;
    SUP_PERMIT_GPIO_BSRR = (1u << (SUP_PIN_PERMIT + 16u));   /* reset the bit */
    SUP_PERMIT_GPIO_MODER =
        (SUP_PERMIT_GPIO_MODER & ~(3u << (SUP_PIN_PERMIT * 2u)))
        | (1u << (SUP_PIN_PERMIT * 2u));                     /* output */

    for (uint32_t *d = &_sdata, *s = &_sidata; d < &_edata; d++, s++) {
        *d = *s;
    }
    for (uint32_t *b = &_sbss; b < &_ebss; b++) {
        *b = 0u;
    }

    (void)main();

    /* main() does not return. If it ever did, stop asserting permission and
     * let the watchdog reset us. */
    SUP_PERMIT_GPIO_BSRR = (1u << (SUP_PIN_PERMIT + 16u));
    default_handler();
}

}  // extern "C"
