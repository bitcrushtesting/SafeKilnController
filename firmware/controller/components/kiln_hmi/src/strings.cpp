/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The display's own words, in both languages.  The conventions, the ASCII
 * transliteration and the 21 character bound are explained in strings.h.
 *
 * The German is written for somebody standing at a kiln, which is why it uses
 * the abbreviations that appear on industrial panels rather than the full words
 * a letter would use: HEIZ, QUIT and STOERUNG are what a German-speaking
 * operator already reads on machinery, and a display is not the place to teach
 * new vocabulary.  Two of them are worth a note:
 *
 *   HOLD -> WARTET.  Holdback is the program waiting for the chamber to catch
 *   up, so "wartet" (waiting) says what is happening.  "HALT" would have been
 *   the literal translation and would have been read as "stopped", which is the
 *   opposite of a kiln that is still firing.
 *
 *   ACK? -> QUIT?  Quittieren is the standard German verb for acknowledging an
 *   alarm on industrial equipment.  It has nothing to do with quitting, and a
 *   German operator will not read it that way.
 *
 * J4 still applies: none of this has been reviewed by a native speaker.
 */

#include "kiln_hmi/strings.h"

namespace {

/* [identifier][language].  A plain table so that the whole translation can be
 * read in one go, which is how a reviewer can actually check it. */
const char *const k_str[KILN_HMI_STR_COUNT][KILN_LANG_COUNT] = {
    /* Screen titles */
    { "MENU",              "MENUE" },
    { "PROGRAMS",          "PROGRAMME" },
    { "NETWORK",           "NETZWERK" },
    { "WIFI NETWORKS",     "WLAN NETZE" },
    { "PASSPHRASE",        "PASSWORT" },
    { "DIAGNOSTICS",       "DIAGNOSE" },
    { "INFO",              "INFO" },
    { "FAULT",             "STOERUNG" },
    { "SUPERVISOR",        "WAECHTER" },

    /* Main screen badges */
    { "HEAT",              "HEIZ" },
    { "HOLD",              "WARTET" },
    { "ACK?",              "QUIT?" },
    { "DOOR",              "TUER" },

    /* Menu */
    { "Start program",     "Programm starten" },
    { "Pause",             "Pause" },
    { "Resume",            "Fortsetzen" },
    { "Pause/Resume",      "Pause/Fortsetzen" },
    { "Abort",             "Abbrechen" },
    { "Erase everything",  "Alles loeschen" },
    { "none stored",       "keine gespeichert" },

    /* Confirmation */
    { "Start firing?",     "Brennen starten?" },
    { "Abort firing?",     "Brennen abbrechen?" },
    { "Erase all data?",   "Alle Daten loeschen?" },
    { "erased",            "geloescht" },
    { "YES",               "JA" },
    { "NO",                "NEIN" },
    { "turn to choose, press", "drehen, dann druecken" },

    /* Network */
    { "connected",         "verbunden" },
    { "not connected",     "nicht verbunden" },
    { "press: set up wifi", "druecken: WLAN einr." },
    { "scanning...",       "suche..." },
    { "none found",        "nichts gefunden" },
    { "hold: back",        "halten: zurueck" },
    { "* needs key",       "* Schluessel" },
    { "of",                "von" },
    { "join failed",       "Verb. gescheitert" },
    { "joining...",        "verbinde..." },

    /* Passphrase entry */
    { "[ delete ]",        "[ loeschen ]" },
    { "[ connect ]",       "[ verbinden ]" },
    { "[ 8 or more ]",     "[ min. 8 ]" },
    { "chars",             "Zeichen" },
    { "hold: cancel",      "halten: Abbruch" },

    /* Diagnostics and info */
    { "case",              "Geh." },
    { "amps",              "Strom" },
    { "duty",              "Takt" },
    { "door",              "Tuer" },
    { "OPEN",              "OFFEN" },
    { "shut",              "zu" },
    { "no switch",         "kein Schalter" },
    { "up",                "Lauf" },
    { "tuned",             "abgestimmt" },
    { "UNTUNED defaults",  "NICHT abgestimmt" },
};

} // namespace

const char *kiln_hmi_str(kiln_hmi_str_id_t id, kiln_lang_t lang)
{
    if ((unsigned)id >= (unsigned)KILN_HMI_STR_COUNT) {
        /* Not an identifier this build knows.  An empty string rather than a
         * guess: a wrong label on a kiln's display is worse than a gap, which
         * at least looks like what it is. */
        return "";
    }
    if ((unsigned)lang >= (unsigned)KILN_LANG_COUNT) {
        lang = KILN_LANG_EN;
    }
    const char *s = k_str[id][lang];
    return (s != nullptr) ? s : k_str[id][KILN_LANG_EN];
}
