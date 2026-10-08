/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The supervisor's self-checks, on the host.
 *
 * The CRC vectors below come from zlib, not from running this implementation
 * and recording the answer. A test written from the implementation agrees with
 * a wrong implementation, and a wrong CRC here would mean a supervisor that
 * refuses to boot on a perfectly good image, or worse, accepts a corrupt one.
 */
#include <string.h>

#include "kiln_check.h"
#include "sup/selfcheck.h"

namespace {

/* SWS must mirror SW, so a healthy CFGR has the same 3-bit value in both. */
uint32_t cfgr(unsigned sw, unsigned sws)
{
    return (sw & 7u) | ((sws & 7u) << SUP_RCC_CFGR_SWS_Pos);
}

const uint32_t CR_OK = SUP_RCC_CR_HSIRDY;      /* HSI ready, PLL off */

}  // namespace

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(selfcheck_a_healthy_clock_tree_passes)
{
    CHECK(sup_clock_ok(CR_OK, cfgr(0, 0)));
    /* Any selection passes as long as the status agrees with it: the check is
     * deliberately independent of the SWS encoding, which this part's SVD does
     * not define. */
    CHECK(sup_clock_ok(CR_OK, cfgr(1, 1)));
    CHECK(sup_clock_ok(CR_OK, cfgr(7, 7)));
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(selfcheck_a_switch_that_did_not_take_is_caught)
{
    /* Software asked for one source and the hardware is running another. Every
     * timer in the firmware is derived from the system clock, so this is the
     * failure that makes the 100 ms cycle and the trip grace periods wrong
     * without anything else looking wrong. */
    CHECK(!sup_clock_ok(CR_OK, cfgr(0, 1)));
    CHECK(!sup_clock_ok(CR_OK, cfgr(2, 0)));
    CHECK(!sup_clock_ok(CR_OK, cfgr(1, 5)));
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(selfcheck_a_stopped_oscillator_and_an_enabled_pll_are_caught)
{
    CHECK(!sup_clock_ok(0u, cfgr(0, 0)));                       /* HSI not ready */
    CHECK(!sup_clock_ok(CR_OK | SUP_RCC_CR_PLLON, cfgr(0, 0))); /* PLL on */
    /* Both wrong at once is still wrong. */
    CHECK(!sup_clock_ok(SUP_RCC_CR_PLLON, cfgr(0, 0)));
}

/*
 * @relation(SWR-TST-09, scope=function)
 */
KILN_TEST(selfcheck_crc32_matches_known_vectors)
{
    /* From zlib. If this implementation and zlib disagree, this implementation
     * is wrong, because the stamping tool uses zlib. */
    CHECK_EQ_UINT(sup_crc32("", 0), 0x00000000u);
    CHECK_EQ_UINT(sup_crc32("a", 1), 0xE8B7BE43u);
    CHECK_EQ_UINT(sup_crc32("123456789", 9), 0xCBF43926u);
    const char *fox = "The quick brown fox jumps over the lazy dog";
    CHECK_EQ_UINT(sup_crc32(fox, strlen(fox)), 0x414FA339u);

    uint8_t all[256];
    for (unsigned i = 0; i < 256u; i++) { all[i] = (uint8_t)i; }
    CHECK_EQ_UINT(sup_crc32(all, sizeof(all)), 0x29058C73u);
}

/*
 * @relation(SWR-TST-09, scope=function)
 */
KILN_TEST(selfcheck_crc32_detects_every_single_bit_flip_in_a_block)
{
    /* The property the check exists for. A CRC that missed a single-bit flip
     * would pass every vector above and still be useless. */
    uint8_t buf[64];
    for (unsigned i = 0; i < sizeof(buf); i++) { buf[i] = (uint8_t)(i * 7u + 1u); }
    const uint32_t good = sup_crc32(buf, sizeof(buf));

    unsigned missed = 0;
    for (unsigned byte = 0; byte < sizeof(buf); byte++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            buf[byte] = (uint8_t)(buf[byte] ^ (1u << bit));
            if (sup_crc32(buf, sizeof(buf)) == good) { missed++; }
            buf[byte] = (uint8_t)(buf[byte] ^ (1u << bit));   /* restore */
        }
    }
    CHECK_EQ_UINT(missed, 0u);
    CHECK_EQ_UINT(sup_crc32(buf, sizeof(buf)), good);          /* fully restored */
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(selfcheck_an_unstamped_image_fails_rather_than_skipping_the_check)
{
    /* The placeholder value must not mean "no expectation, carry on". An image
     * that never went through the stamping step is precisely the one whose
     * integrity is unknown. */
    const char *d = "payload";
    CHECK(!sup_flash_ok(d, 7, SUP_CRC_UNPROGRAMMED));
    CHECK(sup_flash_ok(d, 7, sup_crc32(d, 7)));
}

/*
 * @relation(SWR-SAF-32, scope=function)
 */
KILN_TEST(selfcheck_flash_check_rejects_a_wrong_digest_and_bad_arguments)
{
    const char *d = "payload";
    CHECK(!sup_flash_ok(d, 7, sup_crc32(d, 7) ^ 1u));
    CHECK(!sup_flash_ok(nullptr, 7, 0x12345678u));
    CHECK(!sup_flash_ok(d, 0, 0x12345678u));
    /* A null pointer must not be read, and must not be reported healthy. */
    CHECK_EQ_UINT(sup_crc32(nullptr, 99), 0xFFFFFFFFu);
}


/* --- RAM integrity ------------------------------------------------------ */

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_patterns_drive_every_bit_to_both_states)
{
    /* The property the pattern list exists for. A list that left some bit
     * always zero would pass a block whose cell for that bit is stuck. */
    uint32_t ones = 0, zeros = 0;
    for (unsigned i = 0; i < SUP_RAM_PATTERN_COUNT - 1u; i++) {
        ones  |= SUP_RAM_PATTERNS[i];
        zeros |= ~SUP_RAM_PATTERNS[i];
    }
    CHECK_EQ_UINT(ones, 0xFFFFFFFFu);
    CHECK_EQ_UINT(zeros, 0xFFFFFFFFu);

    /* And the uniform patterns are distinct, or the list is shorter than it
     * looks. */
    for (unsigned i = 0; i < SUP_RAM_PATTERN_COUNT - 1u; i++) {
        for (unsigned j = i + 1u; j < SUP_RAM_PATTERN_COUNT - 1u; j++) {
            CHECK(SUP_RAM_PATTERNS[i] != SUP_RAM_PATTERNS[j]);
        }
    }
}

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_block_passes_on_working_memory_and_is_destructive)
{
    static volatile uint32_t block[32];
    for (unsigned i = 0; i < 32u; i++) { block[i] = 0x12345678u; }

    CHECK(sup_ram_block_ok(block, 32));

    /* Destructive by contract, and the contract matters: a caller that did not
     * save and restore would silently lose whatever lived here. The last thing
     * written is the address-dependent pattern. */
    CHECK_EQ_UINT(block[0], 0u ^ 0xA5A5A5A5u);
    CHECK_EQ_UINT(block[31], 31u ^ 0xA5A5A5A5u);
    CHECK(block[0] != 0x12345678u);
}

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_block_rejects_bad_arguments)
{
    static volatile uint32_t one[1];
    CHECK(!sup_ram_block_ok(nullptr, 4));
    CHECK(!sup_ram_block_ok(one, 0));
    CHECK(sup_ram_block_ok(one, 1));
}

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_word_comparison_is_exact)
{
    /* The comparison is split out because it is the one part a host test can
     * exercise against failures: RAM that can be made to fail on demand is
     * not something a test can arrange. */
    CHECK(sup_ram_word_ok(0xAAAAAAAAu, 0xAAAAAAAAu));
    CHECK(!sup_ram_word_ok(0xAAAAAAAAu, 0xAAAAAAABu));   /* one bit stuck high */
    CHECK(!sup_ram_word_ok(0xFFFFFFFFu, 0x7FFFFFFFu));   /* top bit stuck low  */
    CHECK(!sup_ram_word_ok(0x00000000u, 0x00000001u));
    CHECK(sup_ram_word_ok(0u, 0u));
}

/* --- stack overflow ----------------------------------------------------- */

/*
 * @relation(SWR-SAF-34, scope=function)
 */
KILN_TEST(selfcheck_stack_guard_detects_a_single_word_touched)
{
    static volatile uint32_t guard[16];
    sup_stack_guard_fill(guard, 16);
    CHECK(sup_stack_guard_ok(guard, 16));

    /* A stack that has grown one word into the guard. Every word is checked,
     * not a sample, because the stack arrives at the top of the region and a
     * sampled check would miss a shallow overflow. */
    for (unsigned i = 0; i < 16u; i++) {
        guard[i] = 0u;
        CHECK(!sup_stack_guard_ok(guard, 16));
        guard[i] = SUP_STACK_GUARD_PATTERN;
        CHECK(sup_stack_guard_ok(guard, 16));
    }
}

/*
 * @relation(SWR-SAF-34, scope=function)
 */
KILN_TEST(selfcheck_stack_guard_pattern_is_not_a_value_memory_falls_to)
{
    /* A guard pattern of zero or all-ones would be indistinguishable from
     * uninitialised or erased storage, so the guard would read as intact on a
     * board where it had never been filled. */
    CHECK(SUP_STACK_GUARD_PATTERN != 0x00000000u);
    CHECK(SUP_STACK_GUARD_PATTERN != 0xFFFFFFFFu);

    static volatile uint32_t guard[4] = { 0, 0, 0, 0 };
    CHECK(!sup_stack_guard_ok(guard, 4));        /* never filled, not intact */
    CHECK(!sup_stack_guard_ok(nullptr, 4));
    CHECK(!sup_stack_guard_ok(guard, 0));
    sup_stack_guard_fill(nullptr, 4);            /* must not fault */
}

/* --- program flow ------------------------------------------------------- */

/*
 * @relation(SWR-SAF-35, scope=function)
 */
KILN_TEST(selfcheck_flow_accepts_the_cycle_as_written)
{
    sup_flow_t f;
    sup_flow_begin(&f);
    CHECK(!sup_flow_complete(&f));               /* nothing has run yet */
    sup_flow_mark(&f, SUP_FLOW_READ);
    sup_flow_mark(&f, SUP_FLOW_STEP);
    sup_flow_mark(&f, SUP_FLOW_PERMIT);
    CHECK(!sup_flow_complete(&f));               /* still a stage short */
    sup_flow_mark(&f, SUP_FLOW_REPORT);
    CHECK(sup_flow_complete(&f));
}

/*
 * @relation(SWR-SAF-35, scope=function)
 */
KILN_TEST(selfcheck_flow_catches_a_skipped_stage)
{
    /* The failure a watchdog cannot see: a cycle that finished on time with
     * the permit line never driven. */
    sup_flow_t f;
    sup_flow_begin(&f);
    sup_flow_mark(&f, SUP_FLOW_READ);
    sup_flow_mark(&f, SUP_FLOW_STEP);
    sup_flow_mark(&f, SUP_FLOW_REPORT);          /* PERMIT skipped */
    CHECK(!sup_flow_complete(&f));
}

/*
 * @relation(SWR-SAF-35, scope=function)
 */
KILN_TEST(selfcheck_flow_catches_out_of_order_and_repeated_stages)
{
    sup_flow_t f;
    sup_flow_begin(&f);
    sup_flow_mark(&f, SUP_FLOW_STEP);            /* before READ */
    sup_flow_mark(&f, SUP_FLOW_READ);
    sup_flow_mark(&f, SUP_FLOW_STEP);
    sup_flow_mark(&f, SUP_FLOW_PERMIT);
    sup_flow_mark(&f, SUP_FLOW_REPORT);
    CHECK(!sup_flow_complete(&f));               /* broken stays broken */

    sup_flow_begin(&f);
    sup_flow_mark(&f, SUP_FLOW_READ);
    sup_flow_mark(&f, SUP_FLOW_READ);            /* twice */
    CHECK(!sup_flow_complete(&f));
}

/*
 * @relation(SWR-SAF-35, scope=function)
 */
KILN_TEST(selfcheck_flow_a_fresh_cycle_clears_a_broken_one)
{
    /* Each cycle is judged on its own. A cycle that ran wrongly must not
     * condemn the next one, or one disturbance would latch for ever. The
     * latching is the trip logic's job, not this monitor's. */
    sup_flow_t f;
    sup_flow_begin(&f);
    sup_flow_mark(&f, SUP_FLOW_REPORT);
    CHECK(!sup_flow_complete(&f));

    sup_flow_begin(&f);
    sup_flow_mark(&f, SUP_FLOW_READ);
    sup_flow_mark(&f, SUP_FLOW_STEP);
    sup_flow_mark(&f, SUP_FLOW_PERMIT);
    sup_flow_mark(&f, SUP_FLOW_REPORT);
    CHECK(sup_flow_complete(&f));

    sup_flow_begin(nullptr);                     /* must not fault */
    sup_flow_mark(nullptr, SUP_FLOW_READ);
    CHECK(!sup_flow_complete(nullptr));
}

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_walker_covers_the_window_and_wraps)
{
    uint32_t cursor = 0, addr = 0;
    /* A 64 byte window, 4 word (16 byte) blocks: four blocks then wrap. */
    const uint32_t lo = 0x20001000u, hi = lo + 64u;

    uint32_t seen[4] = { 0, 0, 0, 0 };
    for (unsigned i = 0; i < 4u; i++) {
        CHECK(sup_ram_next_block(lo, hi, 4, &cursor, &addr));
        seen[i] = addr;
    }
    CHECK_EQ_UINT(seen[0], lo);
    CHECK_EQ_UINT(seen[1], lo + 16u);
    CHECK_EQ_UINT(seen[2], lo + 32u);
    CHECK_EQ_UINT(seen[3], lo + 48u);

    /* and back to the bottom, so coverage repeats rather than stopping */
    CHECK(sup_ram_next_block(lo, hi, 4, &cursor, &addr));
    CHECK_EQ_UINT(addr, lo);
}

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_walker_declines_a_window_too_small_to_test)
{
    /* On a target whose stack has grown close to the guard there is nowhere
     * safe to test. Declining is correct; reporting a diagnostic failure for it
     * would be a nuisance trip, and HZ-10 is about what those lead to. */
    uint32_t cursor = 0, addr = 0xDEADu;
    CHECK(!sup_ram_next_block(0x20001000u, 0x20001008u, 4, &cursor, &addr));
    CHECK_EQ_UINT(cursor, 0u);                   /* cursor left alone */
    CHECK(!sup_ram_next_block(0x20001000u, 0x20001000u, 1, &cursor, &addr));
    CHECK(!sup_ram_next_block(0x20001010u, 0x20001000u, 1, &cursor, &addr));
    CHECK(!sup_ram_next_block(0x20001000u, 0x20002000u, 0, &cursor, &addr));
    CHECK(!sup_ram_next_block(0x20001000u, 0x20002000u, 4, nullptr, &addr));
    CHECK(!sup_ram_next_block(0x20001000u, 0x20002000u, 4, &cursor, nullptr));
}

/*
 * @relation(SWR-SAF-33, scope=function)
 */
KILN_TEST(selfcheck_ram_walker_realigns_when_the_window_moves)
{
    /* The upper bound is the stack pointer, so it moves between cycles. A
     * cursor left outside the new window has to restart rather than hand back
     * an address inside the stack. */
    uint32_t cursor = 0, addr = 0;
    const uint32_t lo = 0x20001000u;
    CHECK(sup_ram_next_block(lo, lo + 1024u, 4, &cursor, &addr));
    CHECK(sup_ram_next_block(lo, lo + 1024u, 4, &cursor, &addr));
    CHECK_EQ_UINT(addr, lo + 16u);

    /* the stack grew: the window shrank below the cursor */
    CHECK(sup_ram_next_block(lo, lo + 20u, 4, &cursor, &addr));
    CHECK_EQ_UINT(addr, lo);
    CHECK(addr < lo + 20u);

    /* misaligned bounds clamp inward rather than being rejected */
    CHECK(sup_ram_next_block(lo + 1u, lo + 1023u, 4, &cursor, &addr));
    CHECK_EQ_UINT(addr % 4u, 0u);
    CHECK(addr >= lo + 1u);
}
