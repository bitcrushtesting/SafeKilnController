/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file selfcheck.h
 * @brief The supervisor's self-diagnostics: clock, program memory, RAM, stack
 *        and program sequence.
 *
 * @derivedfrom SWA-22 and SWA-01.
 * @safetyclass The diagnostic measures of EN IEC 60730-1 Annex H for software
 *              class B or C: invariable memory, variable memory, stack,
 *              program sequence and clock. A single-channel supervisor claims
 *              diagnostic coverage only for the failures it can observe, and
 *              this file is what it observes.
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
 *
 * @errorbehaviour
 * Every check here reports a boolean, and a false one reaches the trip logic
 * through `diag_ok` on the input snapshot, which revokes the self-test rather
 * than merely tripping (SWR-SAF-36). That makes it **unclearable by the
 * button**: a supervisor whose memory, stack, clock, image or program sequence
 * has failed is not in the category an operator can acknowledge.
 *
 * @implements SWR-SAF-33, SWR-SAF-34, SWR-SAF-35, SWR-SAF-36
 * @verifiedby `test_selfcheck.cpp`.
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
/**
 * @name Clock register bits (RM0444 RCC_CR and RCC_CFGR)
 * @rangeof Bit positions from ST's CMSIS-SVD, as everything in `stm32g031.h`
 *          is. What is deliberately **not** used is the SWS encoding: the SVD
 *          carries no enumerated values for it and its RCC_CR reset value
 *          disagrees with the reference manual, so every check below is
 *          phrased as an invariant that holds whatever the encoding is.
 * @{
 */
constexpr uint32_t SUP_RCC_CR_HSIRDY   = 1u << 10u; /**< HSI16 is ready. */
constexpr uint32_t SUP_RCC_CR_PLLON    = 1u << 24u; /**< The PLL is enabled. */
constexpr uint32_t SUP_RCC_CFGR_SW_Msk = 7u << 0u;  /**< Clock switch selection. */
constexpr uint32_t SUP_RCC_CFGR_SWS_Pos = 3u;       /**< Clock switch status position. */
constexpr uint32_t SUP_RCC_CFGR_SWS_Msk = 7u << SUP_RCC_CFGR_SWS_Pos; /**< ... and its mask. */
/** @} */

/**
 * @brief Whether the clock tree is the one the firmware was written against.
 *
 * @param[in] rcc_cr   RCC_CR as read.
 * @param[in] rcc_cfgr RCC_CFGR as read.
 * @return True when all three invariants below hold.
 *
 * @rangeof Both arguments are whole register words; bits this does not name
 *          are ignored.
 *
 * @rationale
 * Three invariants, none of which needs the SWS encoding:
 *
 * - **SWS == SW.** The clock switch *status* must agree with the clock switch
 *   *selection*. This catches a switch that did not take effect and a source
 *   that changed underneath us, and it is true for any selection, which is why
 *   it does not need the encoding the SVD does not carry.
 * - **HSIRDY set.** The firmware runs from HSI16 by keeping the reset default,
 *   which `board_clocks_init` states rather than writes, so the oscillator it
 *   depends on has to be running.
 * - **PLLON clear.** `SUP_SYSCLK_HZ` assumes no PLL. A PLL that became enabled
 *   means the system clock is not 16 MHz and every timer derived from it is
 *   wrong.
 *
 * @errorbehaviour
 * False leads to `SUP_TRIP_SELF_TEST`, which never permits heat and which
 * `sup_clear` deliberately refuses to clear: a supervisor that cannot trust
 * its own time base must not be talked back into service by a button.
 *
 * @sideeffects None. Pure function.
 *
 * @implements SWR-SAF-36
 * @verifiedby `test_selfcheck.cpp`: each invariant violated on its own.
 */
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
/**
 * @brief CRC-32 over a buffer, the standard reflected polynomial, in software.
 *
 * @param[in] data Start of the region.
 * @param[in] len  Length in bytes.
 * @return The checksum.
 *
 * @rationale
 * Software rather than the STM32's CRC peripheral on purpose: it makes the
 * computation a pure function that the host tests exercise against known
 * vectors, which is worth more than the handful of microseconds the peripheral
 * would save on a check that runs once per boot over 4 kB.
 *
 * @sideeffects None. Pure function.
 *
 * @verifiedby `test_selfcheck.cpp`, against known vectors.
 */
uint32_t sup_crc32(const void *data, size_t len);

/**
 * @brief What the digest placeholder reads before the build has stamped it.
 *
 * @rangeof 0xFFFFFFFF, which is also what erased flash reads as.
 *
 * @errorbehaviour
 * Treated as a **failure** and not as "skip the check", because an image that
 * reached a board without passing through the build step that stamps it is
 * exactly the image whose integrity is unknown.
 *
 * @implements SWR-SAF-36
 */
constexpr uint32_t SUP_CRC_UNPROGRAMMED = 0xFFFFFFFFu;

/**
 * @brief Whether the program image still hashes to the digest stamped into it.
 *
 * @param[in] data     Start of the region covered, the vector table.
 * @param[in] len      Length of the region, up to `_sup_crc_region_end`.
 * @param[in] expected The stamped digest.
 * @return True when they agree.
 *
 * @rangeof `expected` is a CRC-32, or @ref SUP_CRC_UNPROGRAMMED when the image
 *          was never stamped.
 *
 * @errorbehaviour
 * A corrupted or partially programmed image fails to match and the supervisor
 * comes up refusing heat, rather than running whatever the flash happens to
 * hold. An unstamped image fails too, for the reason given on
 * @ref SUP_CRC_UNPROGRAMMED.
 *
 * @rationale
 * The digest sits immediately after the region because a digest cannot cover
 * itself, and `tools/sup-crc.py` checks that invariant rather than trusting
 * the linker script to maintain it.
 *
 * @resources Runs over roughly 3.5 kB; called once at start-up.
 *
 * @implements SWR-SAF-36
 * @verifiedby `test_selfcheck.cpp`, including the unstamped case.
 */
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
/**
 * @brief How many patterns the RAM test writes.
 *
 * @rangeof 5. Compile-time constant.
 *
 * @rationale
 * Between them every bit of every word is driven to both states, and the
 * **address-dependent** pair catches the failure a simple all-ones and
 * all-zeros pair misses: two words that are physically the same storage
 * because an address line is stuck. Those two pass every uniform pattern,
 * since both hold the same value and both read back what was written, and fail
 * only when asked to hold two different values at once.
 *
 * @implements SWR-SAF-33
 */
constexpr unsigned SUP_RAM_PATTERN_COUNT = 5u;
/* A declaration, not a definition: the array is defined in selfcheck.cpp with
 * its initialiser, so there is no initialisation of any kind in this header to
 * be ordered against anything.  The check reports the extern declaration
 * regardless.
 * NOLINTNEXTLINE(bugprone-dynamic-static-initializers) */
/** @brief The patterns, in order. Defined in `selfcheck.cpp`. */
extern const uint32_t SUP_RAM_PATTERNS[SUP_RAM_PATTERN_COUNT];

/**
 * @brief Whether a block of RAM holds back every pattern written to it.
 *
 * @param[in,out] block Start of the block. **Destructive**: see below.
 * @param[in]     words Length in **words**, not bytes.
 * @return True when every pattern read back as written.
 *
 * @errorbehaviour
 * Destructive by contract: it leaves the block holding the last pattern it
 * wrote, so the caller saves and restores. That is the price of testing the
 * actual storage rather than a copy of it, and it is why the periodic test
 * walks a small block at a time. The block must not overlap the caller's stack
 * or the save buffer it is using.
 *
 * @sideeffects Overwrites `block`.
 *
 * @implements SWR-SAF-33
 * @verifiedby `test_selfcheck.cpp` over the loop; the two failure returns
 *             inside it are unreachable from a host test, because host RAM
 *             cannot be made to fail on demand, which is why the judgement is
 *             split into @ref sup_ram_word_ok and tested there.
 */
bool sup_ram_block_ok(volatile uint32_t *block, size_t words);

/**
 * @brief Whether one word read back as it was written.
 *
 * @param[in] written   The value written.
 * @param[in] read_back The value read.
 * @return True when they are equal.
 *
 * @rationale
 * Split out of @ref sup_ram_block_ok 's loop so the comparison is testable
 * without needing RAM that can be made to fail, which is the one thing a host
 * test cannot arrange. The loop around it is four lines; the judgement in it
 * is tested, for a bit stuck high, a bit stuck low and both polarities of a
 * wrong word.
 *
 * @sideeffects None. Pure function.
 *
 * @implements SWR-SAF-33
 * @verifiedby `test_selfcheck.cpp`.
 */
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
/**
 * @brief Pick the next block to test, walking a window and wrapping at the top.
 *
 * @param[in]     lo       Low bound of the window, word-aligned.
 * @param[in]     hi       High bound, exclusive, word-aligned.
 * @param[in]     words    Block length in words.
 * @param[in,out] cursor   Position in the walk; advanced on success.
 * @param[out]    out_addr Address of the block to test.
 * @return True when a block was chosen.
 * @retval false The window cannot hold a whole block.
 *
 * @rangeof `lo` and `hi` are expected word-aligned. A window that is not is
 *          **clamped inward** rather than rejected, because a misaligned bound
 *          is a linker-script mistake that should degrade to testing less, not
 *          to testing nothing.
 *
 * @errorbehaviour
 * Returning false leaves the cursor alone and is **not** an error: on a target
 * where the stack has grown close to the guard there is simply nowhere safe to
 * test this cycle, and reporting a diagnostic failure for that would be a
 * nuisance trip. A nuisance trip is worse than a missed cycle of a test that
 * runs ten times a second.
 *
 * @rationale
 * Addresses are `uint32_t` rather than pointers so the walk is arithmetic a
 * host test can drive with fabricated addresses. The caller turns the result
 * back into a pointer, which is the one step that needs a real address space.
 *
 * @implements SWR-SAF-33
 * @verifiedby `test_selfcheck.cpp`, including the wrap and the too-small
 *             window.
 */
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
/**
 * @brief The pattern the stack guard is filled with.
 *
 * @rangeof 0xA5C3A5C3. Compile-time constant.
 *
 * @rationale
 * Deliberately not 0x00000000 or 0xFFFFFFFF, because those are what
 * uninitialised or erased storage reads as, and a guard that matches the
 * failure it looks for is no guard at all.
 *
 * @implements SWR-SAF-34
 */
constexpr uint32_t SUP_STACK_GUARD_PATTERN = 0xA5C3A5C3u;

/**
 * @brief Whether every word of the stack guard still holds the pattern.
 *
 * @param[in] guard Start of the guard region.
 * @param[in] words Its length in words.
 * @return True when the stack has not grown into it.
 *
 * @rangeof The region the linker script reserves, 256 words.
 *
 * @errorbehaviour
 * A stack that has grown into the guard has corrupted nothing that matters,
 * because the region is reserved and holds no variables, which is what makes
 * this a warning rather than a post-mortem.
 *
 * @rationale
 * **Every** word is checked, not a sample: the stack arrives at the top of the
 * region, and a sampled check would miss the shallow overflow, which is the
 * one still worth catching.
 *
 * @resources Runs every cycle, ten times a second, over 1 kB.
 *
 * @implements SWR-SAF-34
 * @verifiedby `test_selfcheck.cpp`: a guard with one word disturbed, at both
 *             ends and in the middle.
 */
bool sup_stack_guard_ok(const volatile uint32_t *guard, size_t words);

/**
 * @brief Fill the stack guard with @ref SUP_STACK_GUARD_PATTERN.
 *
 * @param[out] guard Start of the guard region.
 * @param[in]  words Its length in words.
 *
 * @sideeffects Overwrites the region. Called once, at start-up, before the
 *              stack can have grown into it.
 *
 * @implements SWR-SAF-34
 * @verifiedby `test_selfcheck.cpp`, paired with @ref sup_stack_guard_ok.
 */
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
/**
 * @brief The stages of one supervisor cycle, in the order they must run.
 * @implements SWR-SAF-35
 */
typedef enum {
    SUP_FLOW_READ = 0, /**< The thermocouple burst was read. */
    SUP_FLOW_STEP,     /**< The trip logic was stepped. */
    SUP_FLOW_PERMIT,   /**< The permit line was driven. */
    SUP_FLOW_REPORT,   /**< The frame was encoded and queued. */
    SUP_FLOW_COUNT     /**< Count of stages; not a stage. */
} sup_flow_stage_t;

/**
 * @brief Program sequence state for one cycle.
 * @implements SWR-SAF-35
 */
typedef struct {
    uint8_t next;   /**< The stage expected next. */
    bool    broken; /**< A stage arrived out of order, or twice. */
} sup_flow_t;

/**
 * @brief Begin a cycle's sequence monitoring.
 *
 * @param[out] f Sequence state, reset to expect the first stage.
 *
 * @errorbehaviour
 * The verdict necessarily lands in the **following** cycle, since a sequence
 * can only be judged once it has finished, which costs 100 ms against
 * SWR-NFR-04's 500 ms.
 *
 * @implements SWR-SAF-35
 * @verifiedby `test_selfcheck.cpp`.
 */
void sup_flow_begin(sup_flow_t *f);

/**
 * @brief Announce that a stage has run.
 *
 * @param[in,out] f     Sequence state.
 * @param[in]     stage The stage that just completed.
 *
 * @errorbehaviour
 * Announcing a stage out of order, or twice, marks the cycle broken and
 * @ref sup_flow_complete then returns false. The state is not reset by this:
 * a broken cycle stays broken until the next @ref sup_flow_begin.
 *
 * @implements SWR-SAF-35
 * @verifiedby `test_selfcheck.cpp`: skipped, repeated and reordered stages.
 */
void sup_flow_mark(sup_flow_t *f, sup_flow_stage_t stage);

/**
 * @brief Whether every stage ran exactly once and in order.
 *
 * @param[in] f Sequence state at the end of a cycle.
 * @return True only when the cycle did all of its work, in order.
 *
 * @rationale
 * This catches what a watchdog cannot. A watchdog notices a loop that stopped;
 * it cannot notice a loop that ran but skipped the step that drives the permit
 * line, or one that a corrupted branch entered halfway through. Both produce a
 * cycle that finishes on time with the wrong work done.
 *
 * @sideeffects None. Pure function.
 *
 * @implements SWR-SAF-35
 * @verifiedby `test_selfcheck.cpp`.
 */
bool sup_flow_complete(const sup_flow_t *f);

#endif /* SUP_SELFCHECK_H */
