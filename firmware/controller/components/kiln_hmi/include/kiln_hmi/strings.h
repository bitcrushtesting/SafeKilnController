/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The display's own words -- SWR-NFR-23, SWR-HMI-15.
 *
 * `kiln_core/faults` already holds every fault and warning in both languages,
 * which is the prose an operator reads when something has gone wrong.  This is
 * the other half: the screen titles, the menu, the two answers to a
 * confirmation and the footers that say what the knob does.  They were English
 * string literals at their draw sites, so a device configured for German
 * announced its faults in German inside an English frame.
 *
 * ---------------------------------------------------------------------------
 * WHY THE GERMAN IS SPELLED LIKE THAT
 * ---------------------------------------------------------------------------
 * The 5x7 font in draw.cpp carries the 95 printable ASCII glyphs and nothing
 * else, so there are no umlauts and no sharp s to draw with.  Every German
 * string here is therefore transliterated -- AE OE UE SS -- which is what
 * `kiln_core/faults` has always done and is a convention German readers are
 * used to from equipment displays.  A test asserts it, because a single
 * umlaut typed into this table would reach the panel as a blank or a
 * question mark, and nobody would see it until the device was in a workshop.
 *
 * The strings are also bounded: 21 characters is the display's width at
 * scale 1 (128 pixels, 6 per glyph), and a label that overruns it is silently
 * clipped by the framebuffer.  German runs about 15 per cent longer than
 * English, so this is the bound the translation has to be written against
 * rather than a thing to check afterwards -- the test holds every line-length
 * string to it.
 */
#ifndef KILN_HMI_STRINGS_H
#define KILN_HMI_STRINGS_H

#include "kiln/types.h"

/* One identifier per piece of text.  An enum rather than keys, so that adding a
 * screen without translating it does not compile. */
typedef enum {
    /* Screen titles */
    KILN_HMI_STR_MENU = 0,
    KILN_HMI_STR_PROGRAMS,
    KILN_HMI_STR_NETWORK,
    KILN_HMI_STR_NETWORKS,
    KILN_HMI_STR_PASSPHRASE,
    KILN_HMI_STR_DIAGNOSTICS,
    KILN_HMI_STR_INFO,
    KILN_HMI_STR_FAULT,
    KILN_HMI_STR_SUPERVISOR,

    /* The main screen's badges.  Four or five characters: these sit beside one
     * another on one row and are read at a glance from across a workshop. */
    KILN_HMI_STR_HEAT,
    KILN_HMI_STR_HOLD,
    KILN_HMI_STR_ACK,
    /* SWR-SAF-31: shown in the slot that would say HEAT, because an open door
     * is why the kiln is not heating. */
    KILN_HMI_STR_DOOR,

    /* Menu */
    KILN_HMI_STR_START_PROGRAM,
    KILN_HMI_STR_PAUSE,
    KILN_HMI_STR_RESUME,
    KILN_HMI_STR_PAUSE_RESUME,
    KILN_HMI_STR_ABORT,
    KILN_HMI_STR_FACTORY_RESET,
    KILN_HMI_STR_NONE_STORED,

    /* Confirmation */
    KILN_HMI_STR_START_FIRING_Q,
    KILN_HMI_STR_ABORT_FIRING_Q,
    /* SWR-CFG-09.  Phrased as what it destroys rather than as its name: an
     * operator who misreads "factory reset" as "restore defaults" loses their
     * firing history and their WiFi credentials to a single press. */
    KILN_HMI_STR_ERASE_ALL_Q,
    KILN_HMI_STR_ERASED,
    KILN_HMI_STR_YES,
    KILN_HMI_STR_NO,
    KILN_HMI_STR_TURN_THEN_PRESS,

    /* Network */
    KILN_HMI_STR_CONNECTED,
    KILN_HMI_STR_NOT_CONNECTED,
    KILN_HMI_STR_PRESS_SETUP_WIFI,
    KILN_HMI_STR_SCANNING,
    KILN_HMI_STR_NONE_FOUND,
    KILN_HMI_STR_HOLD_BACK,
    KILN_HMI_STR_NEEDS_KEY,
    KILN_HMI_STR_OF,
    KILN_HMI_STR_JOIN_FAILED,
    KILN_HMI_STR_JOINING,

    /* Passphrase entry */
    KILN_HMI_STR_DELETE,
    KILN_HMI_STR_CONNECT,
    KILN_HMI_STR_EIGHT_OR_MORE,
    KILN_HMI_STR_CHARS,
    KILN_HMI_STR_HOLD_CANCEL,

    /* Diagnostics and info.  Column labels, padded to a common width at the
     * draw site rather than here, so the padding is not part of the
     * translation. */
    KILN_HMI_STR_CASE,
    KILN_HMI_STR_AMPS,
    KILN_HMI_STR_DUTY,
    /* The diagnostics row, where all three door states are spelled out. */
    KILN_HMI_STR_DOOR_LABEL,
    KILN_HMI_STR_DOOR_OPEN,
    KILN_HMI_STR_DOOR_SHUT,
    KILN_HMI_STR_DOOR_NONE,
    KILN_HMI_STR_UPTIME,
    KILN_HMI_STR_TUNED,
    KILN_HMI_STR_UNTUNED,

    KILN_HMI_STR_COUNT,
} kiln_hmi_str_id_t;

/* The string, in the configured language.  A language the table does not carry
 * and an identifier out of range both fall back to English rather than to an
 * empty screen, which is what types.h promises for kiln_lang_t. */
const char *kiln_hmi_str(kiln_hmi_str_id_t id, kiln_lang_t lang);

/* The display's width in characters at scale 1, which is the bound every line
 * of text here is written against. */
constexpr unsigned KILN_HMI_COLS = 21u;

#endif /* KILN_HMI_STRINGS_H */
