/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's start-up and periodic self-checks: clock integrity and
 * program-memory integrity.
 *
 * Both are here rather than in main.cpp for the reason the trip logic is: they
 * are decisions about register values and memory contents, they are pure
 * functions of their inputs, and a diagnostic that cannot be exercised on a
 * development host is a diagnostic nobody has tested. main.cpp reads the
 * registers and hands them over; what counts as healthy is decided here.
 *
 * These exist because an assessment against EN IEC 60730-1 Annex H or
 * EN ISO 13849-1 asks for them. Annex H's software classes B and C expect
 * program-memory integrity checking and clock monitoring among their measures,
 * and a single-channel supervisor claims diagnostic coverage only for the
 * failures it can actually observe. The three measures the supervisor now has
 * are this file's two plus the windowed watchdog in main.cpp.
 */
#ifndef SUP_SELFCHECK_H
#define SUP_SELFCHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- clock integrity (RM0444 RCC_CR and RCC_CFGR) -----------------------
 *
 * Bit positions are from ST's CMSIS-SVD for this part, like everything in
 * stm32g031.h. What is deliberately NOT used is the SWS *encoding*: the SVD
 * carries no enumerated values for it, and its RCC_CR reset value disagrees
 * with the reference manual, so every check below is phrased as an invariant
 * that holds whatever the encoding turns out to be.
 */
constexpr uint32_t SUP_RCC_CR_HSIRDY   = 1u << 10u;
constexpr uint32_t SUP_RCC_CR_PLLON    = 1u << 24u;
constexpr uint32_t SUP_RCC_CFGR_SW_Msk = 7u << 0u;
constexpr uint32_t SUP_RCC_CFGR_SWS_Pos = 3u;
constexpr uint32_t SUP_RCC_CFGR_SWS_Msk = 7u << SUP_RCC_CFGR_SWS_Pos;

/* True when the clock tree is the one the firmware was written against.
 *
 * Three invariants, none of which needs the SWS encoding:
 *
 *   SWS == SW.  The clock switch *status* must agree with the clock switch
 *   *selection*. This catches a switch that did not take effect and a source
 *   that changed underneath us, and it is true for any selection.
 *
 *   HSIRDY set. The firmware runs from HSI16 by keeping the reset default
 *   (board_clocks_init states this rather than writing it), so the oscillator
 *   it depends on has to be running.
 *
 *   PLLON clear. SUP_SYSCLK_HZ assumes no PLL. A PLL that became enabled means
 *   the system clock is not 16 MHz, and every timer derived from it is wrong.
 *
 * Returning false leads to SUP_TRIP_SELF_TEST, which never permits heat and
 * which sup_clear deliberately refuses to clear: a supervisor that cannot
 * trust its own time base must not be talked back into service by a button. */
bool sup_clock_ok(uint32_t rcc_cr, uint32_t rcc_cfgr);

/* --- program memory integrity -------------------------------------------
 *
 * CRC-32, the standard reflected polynomial, computed in software.
 *
 * Software rather than the STM32's CRC peripheral on purpose: it makes the
 * computation a pure function that the host tests exercise against known
 * vectors, which is worth more than the handful of microseconds the peripheral
 * would save on a check that runs once per boot over 4 kB.
 */
uint32_t sup_crc32(const void *data, size_t len);

/* True when `len` bytes at `data` hash to `expected`.
 *
 * The expected value is patched into the image after linking by
 * tools/sup-crc.py, over the region the linker script marks. A corrupted or
 * partially-programmed image therefore fails to match and the supervisor comes
 * up refusing heat, rather than running whatever the flash happens to hold.
 *
 * SUP_CRC_UNPROGRAMMED is what the placeholder reads before the tool has run.
 * It is treated as a FAILURE and not as "skip the check", because an image that
 * reached a board without passing through the build step that stamps it is
 * exactly the image whose integrity is unknown. */
constexpr uint32_t SUP_CRC_UNPROGRAMMED = 0xFFFFFFFFu;

bool sup_flash_ok(const void *data, size_t len, uint32_t expected);

/* --- RAM integrity ------------------------------------------------------
 *
 * A pattern test over a block of RAM. DESTRUCTIVE by contract: it leaves the
 * block holding the last pattern it wrote, so the caller saves and restores.
 * That is the price of testing the actual storage rather than a copy of it,
 * and it is why the periodic test walks a small block at a time.
 */

/* The patterns, in order. Between them every bit of every word is driven to
 * both states, and the address-dependent pair catches the failure a simple
 * all-ones/all-zeros pair misses: two words that are physically the same
 * storage because an address line is stuck. */
constexpr unsigned SUP_RAM_PATTERN_COUNT = 5u;
/* A declaration, not a definition: the array is defined in selfcheck.cpp with
 * its initialiser, so there is no initialisation of any kind in this header to
 * be ordered against anything.  The check reports the extern declaration
 * regardless.
 * NOLINTNEXTLINE(bugprone-dynamic-static-initializers) */
extern const uint32_t SUP_RAM_PATTERNS[SUP_RAM_PATTERN_COUNT];

/* True when the block holds every pattern written to it.
 *
 * `words` is a word count, not a byte count. The block must not overlap the
 * caller's stack or the save buffer it is using, for the obvious reason. */
bool sup_ram_block_ok(volatile uint32_t *block, size_t words);

/* True when `read_back` is what `written` should have produced.
 *
 * Split out from the loop so the comparison is testable without needing RAM
 * that can be made to fail, which is the one thing a host test cannot arrange. */
bool sup_ram_word_ok(uint32_t written, uint32_t read_back);

/* Pick the next block to test, advancing `cursor` through [lo, hi) and
 * wrapping at the top.
 *
 * Addresses are uint32_t rather than pointers so the walk is arithmetic a host
 * test can drive with fabricated addresses. The caller turns the result back
 * into a pointer, which is the one step that needs a real address space.
 *
 * Returns false, and leaves the cursor alone, when the window cannot hold a
 * whole block. That is not an error: on a target where the stack has grown
 * close to the guard there is simply nowhere safe to test this cycle, and
 * reporting a diagnostic failure for it would be a nuisance trip.
 *
 * `lo` and `hi` are expected word-aligned; a window that is not is clamped
 * inward rather than rejected, because a misaligned bound is a linker-script
 * mistake that should degrade to testing less, not to testing nothing. */
bool sup_ram_next_block(uint32_t lo, uint32_t hi, size_t words,
                        uint32_t *cursor, uint32_t *out_addr);

/* --- stack overflow -----------------------------------------------------
 *
 * A guard region below the stack, filled with a known pattern at start-up and
 * checked every cycle. A stack that has grown into it has already corrupted
 * nothing that matters, because the region is reserved and holds no variables,
 * which is what makes this a warning rather than a post-mortem.
 *
 * The linker script reserves the region; the pattern is deliberately not
 * 0x00000000 or 0xFFFFFFFF, because those are what uninitialised or erased
 * storage reads as and a guard that matches the failure it looks for is no
 * guard at all.
 */
constexpr uint32_t SUP_STACK_GUARD_PATTERN = 0xA5C3A5C3u;

/* True when every word of the guard still holds the pattern. */
bool sup_stack_guard_ok(const volatile uint32_t *guard, size_t words);

void sup_stack_guard_fill(volatile uint32_t *guard, size_t words);

/* --- program flow -------------------------------------------------------
 *
 * Sequence monitoring: each stage of the cycle announces itself, and at the end
 * of the cycle the supervisor checks that every stage ran, once, in order.
 *
 * This catches what a watchdog cannot. A watchdog notices a loop that stopped;
 * it cannot notice a loop that ran but skipped the step that drives the permit
 * line, or one that a corrupted branch entered halfway through. Both of those
 * produce a cycle that finishes on time with the wrong work done.
 */
typedef enum {
    SUP_FLOW_READ = 0,      /* the thermocouple burst was read               */
    SUP_FLOW_STEP,          /* the trip logic was stepped                    */
    SUP_FLOW_PERMIT,        /* the permit line was driven                    */
    SUP_FLOW_REPORT,        /* the frame was encoded and queued              */
    SUP_FLOW_COUNT
} sup_flow_stage_t;

typedef struct {
    uint8_t next;           /* the stage expected next                       */
    bool    broken;         /* a stage arrived out of order, or twice        */
} sup_flow_t;

void sup_flow_begin(sup_flow_t *f);

/* Announce a stage. Announcing one out of order, or twice, breaks the cycle. */
void sup_flow_mark(sup_flow_t *f, sup_flow_stage_t stage);

/* True only when every stage ran exactly once and in order. */
bool sup_flow_complete(const sup_flow_t *f);

#endif /* SUP_SELFCHECK_H */
