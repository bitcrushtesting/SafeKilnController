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
