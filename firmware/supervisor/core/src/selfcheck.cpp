/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "sup/selfcheck.h"

bool sup_clock_ok(uint32_t rcc_cr, uint32_t rcc_cfgr)
{
    const uint32_t sw  = rcc_cfgr & SUP_RCC_CFGR_SW_Msk;
    const uint32_t sws = (rcc_cfgr & SUP_RCC_CFGR_SWS_Msk) >> SUP_RCC_CFGR_SWS_Pos;

    if (sws != sw) {
        return false;       /* the switch did not take, or something moved it */
    }
    if ((rcc_cr & SUP_RCC_CR_HSIRDY) == 0u) {
        return false;       /* the oscillator we time from is not running */
    }
    if ((rcc_cr & SUP_RCC_CR_PLLON) != 0u) {
        return false;       /* the system clock is not the 16 MHz assumed */
    }
    return true;
}

uint32_t sup_crc32(const void *data, size_t len)
{
    /* Bitwise rather than table-driven: 256 words of table would be a quarter
     * of this firmware's RAM budget or a sixteenth of its flash, to speed up a
     * check that runs once per boot. The loop is 4 kB * 8 iterations, which at
     * 16 MHz is under ten milliseconds and happens before the first permit. */
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t crc = 0xFFFFFFFFu;
    if (p == nullptr) {
        return crc;
    }
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint32_t>(p[i]);
        for (unsigned b = 0; b < 8u; b++) {
            /* The reflected CRC-32 polynomial, 0xEDB88320. */
            const uint32_t mask = (crc & 1u) != 0u ? 0xEDB88320u : 0u;
            crc = (crc >> 1u) ^ mask;
        }
    }
    return ~crc;
}

bool sup_flash_ok(const void *data, size_t len, uint32_t expected)
{
    if (expected == SUP_CRC_UNPROGRAMMED) {
        return false;       /* never stamped; integrity is unknown, not fine */
    }
    if ((data == nullptr) || (len == 0u)) {
        return false;
    }
    return sup_crc32(data, len) == expected;
}

/* --- RAM integrity ------------------------------------------------------ */

const uint32_t SUP_RAM_PATTERNS[SUP_RAM_PATTERN_COUNT] = {
    0x00000000u,
    0xFFFFFFFFu,
    0xAAAAAAAAu,
    0x55555555u,
    /* The address-dependent one is written as a marker and replaced per word
     * inside the loop; this entry reserves its place in the order so the count
     * and the sequence are stated in one place. */
    0xDEADBEEFu,
};

bool sup_ram_word_ok(uint32_t written, uint32_t read_back)
{
    return written == read_back;
}

bool sup_ram_block_ok(volatile uint32_t *block, size_t words)
{
    if ((block == nullptr) || (words == 0u)) {
        return false;
    }

    /* Uniform patterns first: every bit to both states, one word at a time so
     * a failure is attributed to the word that failed. */
    for (unsigned p = 0; p < SUP_RAM_PATTERN_COUNT - 1u; p++) {
        const uint32_t pat = SUP_RAM_PATTERNS[p];
        for (size_t i = 0; i < words; i++) {
            block[i] = pat;
        }
        for (size_t i = 0; i < words; i++) {
            if (!sup_ram_word_ok(pat, block[i])) {
                return false;
            }
        }
    }

    /* Then an address-dependent pattern, written across the whole block before
     * any of it is read back. This is the one that catches a stuck address
     * line: two words that are physically the same storage pass every uniform
     * pattern and fail here, because they would have to hold two different
     * values at once. */
    for (size_t i = 0; i < words; i++) {
        block[i] = static_cast<uint32_t>(i) ^ 0xA5A5A5A5u;
    }
    for (size_t i = 0; i < words; i++) {
        const uint32_t want = static_cast<uint32_t>(i) ^ 0xA5A5A5A5u;
        if (!sup_ram_word_ok(want, block[i])) {
            return false;
        }
    }
    return true;
}

bool sup_ram_next_block(uint32_t lo, uint32_t hi, size_t words,
                        uint32_t *cursor, uint32_t *out_addr)
{
    if ((cursor == nullptr) || (out_addr == nullptr) || (words == 0u)) {
        return false;
    }
    /* Clamp inward to word alignment: a misaligned bound degrades to testing
     * slightly less rather than to testing nothing. */
    const uint32_t a_lo = (lo + 3u) & ~3u;
    const uint32_t a_hi = hi & ~3u;
    if (a_hi <= a_lo) {
        return false;
    }
    const uint32_t span = a_hi - a_lo;
    const uint32_t need = static_cast<uint32_t>(words) * 4u;
    if (span < need) {
        return false;               /* nowhere safe to test this cycle */
    }
    uint32_t c = *cursor;
    if ((c < a_lo) || (c > a_hi - need)) {
        c = a_lo;                   /* first call, or the window moved */
    }
    *out_addr = c;
    c += need;
    /* Wrap when the next block would not fit, so coverage is repeated rather
     * than stopping at the top. */
    *cursor = (c > a_hi - need) ? a_lo : c;
    return true;
}

/* --- stack overflow ----------------------------------------------------- */

void sup_stack_guard_fill(volatile uint32_t *guard, size_t words)
{
    if (guard == nullptr) {
        return;
    }
    for (size_t i = 0; i < words; i++) {
        guard[i] = SUP_STACK_GUARD_PATTERN;
    }
}

bool sup_stack_guard_ok(const volatile uint32_t *guard, size_t words)
{
    if ((guard == nullptr) || (words == 0u)) {
        return false;
    }
    for (size_t i = 0; i < words; i++) {
        if (guard[i] != SUP_STACK_GUARD_PATTERN) {
            return false;
        }
    }
    return true;
}

/* --- program flow ------------------------------------------------------- */

void sup_flow_begin(sup_flow_t *f)
{
    if (f == nullptr) {
        return;
    }
    f->next   = 0u;
    f->broken = false;
}

void sup_flow_mark(sup_flow_t *f, sup_flow_stage_t stage)
{
    if (f == nullptr) {
        return;
    }
    /* Out of order, repeated, or past the end: all the same verdict, because
     * all of them mean the cycle did not run the way it was written. */
    if (static_cast<unsigned>(stage) != f->next) {
        f->broken = true;
        return;
    }
    f->next = static_cast<uint8_t>(f->next + 1u);
}

bool sup_flow_complete(const sup_flow_t *f)
{
    if (f == nullptr) {
        return false;
    }
    return !f->broken && (f->next == static_cast<unsigned>(SUP_FLOW_COUNT));
}
