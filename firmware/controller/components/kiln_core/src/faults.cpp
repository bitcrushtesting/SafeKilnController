/* SPDX-FileCopyrightText: 2026 Bitcrush Testing
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "kiln_core/faults.h"

#include <cstddef>
#include <iterator>

namespace {

/* Each entry carries its own code, so the table's index relationship can be
 * asserted at compile time.  C++ has no array designators, and the bare
 * positional array a straight translation would give silently shifts every
 * entry if an enumerator is ever inserted in the middle -- precisely the
 * failure the C designators existed to prevent.  The static_asserts below are
 * strictly stronger than those designators were: they check placement,
 * completeness, and that nobody has renumbered the enum, all before the image
 * is built rather than in a unit test that has to be remembered. */
/* SWR-NFR-23: label and cause per language, indexed by kiln_lang_t.  Adding a
 * language is a column here and nowhere else, which is the single resource
 * location the requirement asks for.  `req` is not translated: it is a
 * requirement identifier, and translating "SWR-SAF-07" would be a defect. */
typedef struct {
    kiln_fault_t code;
    const char  *label[KILN_LANG_COUNT];
    const char  *cause[KILN_LANG_COUNT];
    const char  *req;
} fault_entry_t;

typedef struct {
    kiln_warn_bit_t code;
    const char     *label[KILN_LANG_COUNT];
    const char     *cause[KILN_LANG_COUNT];
    const char     *req;
} warn_entry_t;

/* Indexed by kiln_fault_t; order is load-bearing and checked by a unit test. */
constexpr fault_entry_t k_faults[] = {
    { KILN_FAULT_NONE,
        { "OK", "OK" },
        { "No fault",
          "Keine Störung" },
        "" },
    { KILN_FAULT_TC_OPEN,
        { "TC OPEN", "TE OFFEN" },
        { "Kiln thermocouple open circuit. Check the probe and its connections.",
          "Thermoelement des Ofens unterbrochen. Fühler und Anschlüsse prüfen." },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_SHORT,
        { "TC SHORT", "TE KURZ" },
        { "Kiln thermocouple shorted. Check for damaged insulation.",
          "Thermoelement des Ofens kurzgeschlossen. Auf beschädigte Isolierung prüfen." },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_RANGE,
        { "TC RANGE", "TE BEREICH" },
        { "Kiln temperature outside the valid range for this thermocouple type.",
          "Ofentemperatur außerhalb des gültigen Bereichs für diesen Thermoelementtyp." },
        "SWR-SAF-04" },
    { KILN_FAULT_CJ,
        { "CJ FAULT", "VERGLEICH" },
        { "Cold junction out of range. The controller itself may be too hot or too cold.",
          "Vergleichsstelle außerhalb des Bereichs. Der Regler selbst ist möglicherweise zu heiß oder zu kalt." },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_COMMS,
        { "TC COMMS", "TE KOMM" },
        { "No response from the thermocouple front end. Check wiring to the MAX31856.",
          "Keine Antwort vom Thermoelement-Messeingang. Verdrahtung zum MAX31856 prüfen." },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_REVERSED,
        { "TC REVERSED", "TE VERPOLT" },
        { "Temperature fell while heating at high duty: the thermocouple is probably connected with reversed polarity.",
          "Temperatur fiel bei hoher Heizleistung: das Thermoelement ist vermutlich verpolt angeschlossen." },
        "SWR-SAF-05" },
    { KILN_FAULT_TC_STUCK,
        { "TC STUCK", "TE FEST" },
        { "Temperature did not change while heating. The probe reading is stuck.",
          "Temperatur änderte sich trotz Heizen nicht. Der Messwert hängt fest." },
        "SWR-SAF-06" },
    { KILN_FAULT_RUNAWAY,
        { "NO HEAT", "KEINE WAERME" },
        { "Heating at high duty produced no temperature rise: failed element, open contactor, open safety chain, or the lid is not closed.",
          "Heizen mit hoher Leistung brachte keinen Temperaturanstieg: defekte Heizwendel, offenes Schütz, unterbrochene Sicherheitskette oder der Deckel ist nicht geschlossen." },
        "SWR-SAF-07" },
    { KILN_FAULT_UNCOMMANDED_HEAT,
        { "SSR SHORT", "SSR KURZ" },
        { "Temperature rising with heating commanded off. The SSR is probably shorted. Isolate the kiln at the wall.",
          "Temperatur steigt, obwohl die Heizung ausgeschaltet ist. Das SSR ist vermutlich durchlegiert. Ofen allpolig vom Netz trennen." },
        "SWR-SAF-08" },
    { KILN_FAULT_OVERTEMP,
        { "OVERTEMP", "UEBERTEMP" },
        { "Kiln temperature exceeded the configured maximum.",
          "Ofentemperatur hat das eingestellte Maximum überschritten." },
        "SWR-SAF-09" },
    { KILN_FAULT_SP_EXCURSION,
        { "OVERSHOOT", "UEBERSCHWING" },
        { "Temperature ran far above setpoint for too long. PID gains may be wrong.",
          "Temperatur lag zu lange deutlich über dem Sollwert. Die PID-Parameter sind möglicherweise falsch." },
        "SWR-SAF-10" },
    { KILN_FAULT_CASE_OVERTEMP,
        { "CASE HOT", "GEHAEUSE HEISS" },
        { "Controller enclosure too hot. Improve ventilation or move the controller.",
          "Reglergehäuse zu heiß. Belüftung verbessern oder den Regler versetzen." },
        "SWR-SAF-11" },
    { KILN_FAULT_CONTROL_DEADLINE,
        { "CTRL LATE", "REGLER SPAET" },
        { "The control loop missed its deadline. This is a firmware fault.",
          "Der Regelkreis hat seine Frist überschritten. Das ist ein Firmwarefehler." },
        "SWR-SAF-13" },
    { KILN_FAULT_SAFETY_DEADLINE,
        { "SAFE LATE", "SICH SPAET" },
        { "The safety supervisor missed its deadline. This is a firmware fault.",
          "Die Sicherheitsüberwachung hat ihre Frist überschritten. Das ist ein Firmwarefehler." },
        "SWR-SAF-13" },
    { KILN_FAULT_WATCHDOG,
        { "WATCHDOG", "WATCHDOG" },
        { "The controller was reset by the watchdog during a firing.",
          "Der Regler wurde während eines Brandes vom Watchdog zurückgesetzt." },
        "SWR-SAF-14" },
    { KILN_FAULT_CASE_TC,
        { "CASE TC", "GEH TE" },
        { "Enclosure thermocouple fault. Enclosure temperature cannot be supervised.",
          "Störung am Gehäuse-Thermoelement. Die Gehäusetemperatur kann nicht überwacht werden." },
        "SWR-SAF-04" },
    { KILN_FAULT_OPERATOR_ABORT,
        { "ABORTED", "ABGEBROCHEN" },
        { "Firing aborted by the operator.",
          "Brand vom Bediener abgebrochen." },
        "SWR-RUN-04" },
    { KILN_FAULT_TUNE_NO_CONVERGE,
        { "TUNE FAIL", "TUNING FEHL" },
        { "Automatic tuning did not find a stable oscillation. Previous gains kept.",
          "Die automatische Abstimmung fand keine stabile Schwingung. Die bisherigen Parameter bleiben erhalten." },
        "SWR-TUN-07" },
    { KILN_FAULT_RECOVERY_REFUSED,
        { "NO RESUME", "KEIN RESTART" },
        { "Power was lost during a firing and the kiln had cooled too far to resume safely.",
          "Während des Brandes fiel die Spannung aus und der Ofen war zu weit abgekühlt, um sicher fortzusetzen." },
        "SWR-RUN-08" },
    { KILN_FAULT_CONFIG_STORAGE,
        { "STORAGE", "SPEICHER" },
        { "Configuration could not be read or written. Defaults are in use.",
          "Die Konfiguration konnte nicht gelesen oder geschrieben werden. Es gelten die Standardwerte." },
        "SWR-CFG-05" },
    { KILN_FAULT_UNCOMMANDED_CURRENT,
        { "RELAY ON", "RELAIS EIN" },
        { "Heater current is flowing with heating commanded off. A relay is stuck on.",
          "Es fließt Heizstrom, obwohl die Heizung ausgeschaltet ist. Ein Schaltglied hängt." },
        "SWR-SAF-25" },
    { KILN_FAULT_CONTACTOR_WELDED,
        { "WELDED", "VERSCHWEISST" },
        { "Current continued after the contactor was commanded open: the contactor is welded shut. The controller can no longer interrupt the heaters. ISOLATE THE KILN AT ITS SUPPLY NOW.",
          "Der Strom floss weiter, nachdem das Schütz geöffnet werden sollte: das Schütz ist verschweißt. Der Regler kann die Heizung nicht mehr abschalten. OFEN SOFORT ALLPOLIG VOM NETZ TRENNEN." },
        "SWR-SAF-27" },
    { KILN_FAULT_NO_HEATER_CURRENT,
        { "NO CURRENT", "KEIN STROM" },
        { "No heater current with heating commanded on: failed SSR, open contactor, blown heater fuse, open safety chain or open elements.",
          "Kein Heizstrom bei eingeschalteter Heizung: defektes SSR, offenes Schütz, durchgebrannte Heizungssicherung, unterbrochene Sicherheitskette oder offene Heizwendeln." },
        "SWR-SAF-26" },
    { KILN_FAULT_CURRENT_DEVIATION,
        { "CURR DEV", "STROM ABW" },
        { "Heater current differs sharply from this run's reference. An element group has probably failed.",
          "Der Heizstrom weicht stark vom Referenzwert dieses Brandes ab. Vermutlich ist eine Heizgruppe ausgefallen." },
        "SWR-SAF-28" },
    { KILN_FAULT_OVERCURRENT,
        { "OVERCURRENT", "UEBERSTROM" },
        { "Heater current above the configured maximum: shorted element, wrong wiring or a failed SSR.",
          "Heizstrom über dem eingestellten Maximum: Windungsschluss, falsche Verdrahtung oder ein defektes SSR." },
        "SWR-SAF-29" },
    { KILN_FAULT_CT_FAULT,
        { "CT FAULT", "WANDLER" },
        { "No signal from the current transformer. Check that it is fitted around one heater conductor and plugged in.",
          "Kein Signal vom Stromwandler. Prüfen, ob er um einen Heizleiter gelegt und angeschlossen ist." },
        "SWR-CUR-11" },
    { KILN_FAULT_DOOR_OPEN,
        { "DOOR OPEN", "TUER OFFEN" },
        { "The kiln door was opened during a firing. Heating stopped immediately. Close the door and acknowledge to continue.",
          "Die Ofentür wurde während eines Brandes geöffnet. Die Heizung wurde sofort abgeschaltet. Tür schließen und quittieren, um fortzufahren." },
        "SWR-SAF-31" },
};

constexpr warn_entry_t k_warns[] = {
    { KILN_WARN_INSULATION,
        { "INSULATION", "ISOLIERUNG" },
        { "This firing needed noticeably more energy than usual. Insulation or elements may be degrading.",
          "Dieser Brand benötigte spürbar mehr Energie als üblich. Isolierung oder Heizwendeln könnten nachlassen." },
        "SWR-SAF-12" },
    { KILN_WARN_HOLDBACK,
        { "HOLDBACK", "HALTEN" },
        { "The kiln is behind its curve, so the schedule is being held back.",
          "Der Ofen liegt hinter seiner Kurve, daher wird der Zeitplan zurückgehalten." },
        "SWR-CTL-11" },
    { KILN_WARN_LOG_UNAVAIL,
        { "NO LOG", "KEIN LOG" },
        { "Temperature logging is unavailable. The firing continues.",
          "Die Temperaturaufzeichnung ist nicht verfügbar. Der Brand läuft weiter." },
        "SWR-LOG-14" },
    { KILN_WARN_DISPLAY_UNAVAIL,
        { "NO DISPLAY", "KEIN DISPLAY" },
        { "The display is not responding. The firing continues.",
          "Das Display antwortet nicht. Der Brand läuft weiter." },
        "SWR-HMI-14" },
    { KILN_WARN_TIME_UNSYNCED,
        { "NO TIME", "KEINE ZEIT" },
        { "Clock not synchronised; log timestamps are relative only.",
          "Uhr nicht synchronisiert; Zeitstempel im Log sind nur relativ." },
        "SWR-LOG-12" },
    { KILN_WARN_WIFI_DOWN,
        { "NO WIFI", "KEIN WLAN" },
        { "WiFi is down. The firing is unaffected.",
          "WLAN ist nicht verbunden. Der Brand ist davon nicht betroffen." },
        "SWR-NET-07" },
    { KILN_WARN_DUTY_SATURATED,
        { "FULL POWER", "VOLLLAST" },
        { "Heating has been at full power for a long time; the kiln may not keep up.",
          "Die Heizung läuft seit Längerem auf Volllast; der Ofen kommt möglicherweise nicht nach." },
        "SWR-CTL-15" },
    { KILN_WARN_GAINS_UNTUNED,
        { "UNTUNED", "UNGETUNT" },
        { "PID gains are factory defaults. Run automatic tuning for this kiln.",
          "Die PID-Parameter sind Werkseinstellungen. Automatische Abstimmung für diesen Ofen durchführen." },
        "SWR-TUN-11" },
    { KILN_WARN_RELAY_WEAR,
        { "RELAY WEAR", "RELAIS VERSCHL" },
        { "A relay has reached its configured switching-operation life limit. Plan to replace it.",
          "Ein Schaltglied hat seine eingestellte Schaltspielzahl erreicht. Austausch einplanen." },
        "SWR-SAF-30" },
    { KILN_WARN_RELAY_SUSPECT,
        { "RELAY?", "RELAIS?" },
        { "Heater current has intermittently disagreed with the commanded state. A relay may be starting to fail.",
          "Der Heizstrom wich zeitweise vom Schaltzustand ab. Ein Schaltglied könnte ausfallen." },
        "SWR-SAF-30" },
    { KILN_WARN_CURRENT_OFF,
        { "NO CT", "KEIN WANDLER" },
        { "Current monitoring is disabled. Relay faults can only be inferred from temperature, which is far slower.",
          "Die Stromüberwachung ist abgeschaltet. Relaisfehler lassen sich dann nur über die Temperatur erkennen, was weit langsamer ist." },
        "SWR-CUR-12" },
    { KILN_WARN_CURRENT_DEV,
        { "CURR LOW", "STROM NIEDRIG" },
        { "Heater current is drifting from this run's reference. Elements may be ageing.",
          "Der Heizstrom driftet vom Referenzwert dieses Brandes ab. Die Heizwendeln könnten altern." },
        "SWR-SAF-28" },
    { KILN_WARN_DOOR_OFF,
        { "NO DOOR SW", "KEIN TUERSCH" },
        { "No door interlock is fitted. Opening the door during a firing will not stop the heater.",
          "Es ist kein Türkontakt eingebaut. Ein Öffnen der Tür während eines Brandes schaltet die Heizung nicht ab." },
        "SWR-SAF-31" },
};

/* The two tables are indexed by their code, and that is proven here rather
 * than trusted.  Inserting an enumerator in the middle, renumbering one, or
 * forgetting to add a row all fail the build. */
template <typename E, typename T, std::size_t N>
constexpr bool indexed_by_code(const T (&table)[N])
{
    for (std::size_t i = 0; i < N; ++i) {
        if (static_cast<std::size_t>(table[i].code) != i) { return false; }
    }
    return true;
}

} // namespace

namespace {

/* SWA-22.  Indexed by kiln_sup_reason_t; the static_assert below holds the
 * table to the enum, as the fault tables are held to theirs.
 *
 * The cause text says what to *do*, not only what happened, because the
 * supervisor's latch is cleared by the button on the panel and not by the
 * acknowledgement an operator has learned for every other fault.  Two
 * acknowledgements exist and the operator did not choose that. */
constexpr struct {
    kiln_sup_reason_t code;
    const char *label[KILN_LANG_COUNT];
    const char *cause[KILN_LANG_COUNT];
} k_sup_reasons[] = {
    { KILN_SUP_OK,
        { "OK", "OK" },
        { "The independent supervisor is permitting heat.",
          "Der unabhaengige Waechter gibt die Heizung frei." } },
    { KILN_SUP_OVERTEMP,
        { "SUPERVISOR OVER-TEMP", "WAECHTER UEBERTEMPERATUR" },
        { "The independent supervisor stopped the kiln above its own fixed "
          "limit, which sits above every configurable one. The controller "
          "should have stopped first, so treat this as a controller fault as "
          "well. Press the supervisor's clear button on the panel.",
          "Der unabhaengige Waechter hat den Ofen oberhalb seiner eigenen "
          "festen Grenze abgeschaltet, die ueber allen einstellbaren liegt. "
          "Die Steuerung haette vorher abschalten muessen, also auch als "
          "Fehler der Steuerung behandeln. Quittiertaste des Waechters am "
          "Bedienfeld druecken." } },
    { KILN_SUP_TC_FAULT,
        { "SUPERVISOR TC FAULT", "WAECHTER FUEHLERFEHLER" },
        { "The independent supervisor's thermocouple front end reported a "
          "fault. Check the chamber probe and its wiring, then press the "
          "supervisor's clear button on the panel.",
          "Die Fuehlerelektronik des unabhaengigen Waechters meldet einen "
          "Fehler. Kammerfuehler und Verdrahtung pruefen, dann Quittiertaste "
          "des Waechters am Bedienfeld druecken." } },
    { KILN_SUP_SENSOR_STALE,
        { "SUPERVISOR NO READING", "WAECHTER KEIN MESSWERT" },
        { "The independent supervisor was reading the chamber and stopped "
          "getting a value. Check the probe and its wiring, then press the "
          "supervisor's clear button on the panel.",
          "Der unabhaengige Waechter hat die Kammer gemessen und erhaelt "
          "keinen Wert mehr. Fuehler und Verdrahtung pruefen, dann "
          "Quittiertaste des Waechters am Bedienfeld druecken." } },
    { KILN_SUP_SELF_TEST,
        { "SUPERVISOR SELF-TEST", "WAECHTER SELBSTTEST" },
        { "The independent supervisor could not configure its thermocouple "
          "front end at start-up and will not permit heat. This is not "
          "clearable by the operator; the supervisor needs servicing.",
          "Der unabhaengige Waechter konnte seine Fuehlerelektronik beim Start "
          "nicht konfigurieren und gibt die Heizung nicht frei. Nicht durch "
          "den Bediener quittierbar; der Waechter muss instand gesetzt "
          "werden." } },
    { KILN_SUP_LINK_DEAD,
        { "NO SUPERVISOR", "KEIN WAECHTER" },
        { "Nothing is arriving from the independent supervisor, so there is no "
          "chamber temperature and no heat. This is the absence of the "
          "backstop rather than the backstop acting: check that the supervisor "
          "is powered and its link is connected.",
          "Vom unabhaengigen Waechter kommt nichts an, daher gibt es keine "
          "Kammertemperatur und keine Heizung. Das ist das Fehlen der "
          "Rueckfallebene, nicht ihr Ansprechen: pruefen, ob der Waechter "
          "versorgt und die Verbindung angeschlossen ist." } },
    { KILN_SUP_TC_DISAGREE,
        { "SUPERVISOR PROBES DISAGREE", "WAECHTER FUEHLER UNEINIG" },
        { "The supervisor's two chamber probes are reporting temperatures too "
          "far apart to be measuring the same thing, so one of them is wrong "
          "and there is no way to tell which. Check both probes and their "
          "wiring, then press the supervisor's clear button on the panel.",
          "Die zwei Kammerfuehler des Waechters melden zu weit "
          "auseinanderliegende Temperaturen, um dasselbe zu messen; einer ist "
          "also falsch und es ist nicht erkennbar welcher. Beide Fuehler und "
          "ihre Verdrahtung pruefen, dann Quittiertaste des Waechters am "
          "Bedienfeld druecken." } },
    { KILN_SUP_OUTPUT_STUCK,
        { "SUPERVISOR CANNOT CUT POWER", "WAECHTER KANN NICHT ABSCHALTEN" },
        { "The supervisor withdrew permission to heat and the coil stayed "
          "energised, so it cannot interrupt the heater. ISOLATE THE KILN AT "
          "ITS SUPPLY. Do not rely on the controller or on the supervisor's "
          "clear button; the switching hardware needs attention before the kiln "
          "is used again.",
          "Der Waechter hat die Heizfreigabe entzogen und die Spule blieb "
          "bestromt, er kann die Heizung also nicht unterbrechen. OFEN AN DER "
          "ZULEITUNG FREISCHALTEN. Nicht auf Steuerung oder Quittiertaste "
          "verlassen; die Schalttechnik muss geprueft werden, bevor der Ofen "
          "wieder benutzt wird." } },
};

}  // namespace

static_assert(std::size(k_sup_reasons) == KILN_SUP_REASON_COUNT,
              "k_sup_reasons must have one row per kiln_sup_reason_t");
static_assert(indexed_by_code<kiln_sup_reason_t>(k_sup_reasons),
              "k_sup_reasons must be indexed by kiln_sup_reason_t");

static_assert(std::size(k_faults) == KILN_FAULT_MAX,
              "k_faults must have one row per kiln_fault_t");
static_assert(indexed_by_code<kiln_fault_t>(k_faults),
              "k_faults must be indexed by kiln_fault_t");
static_assert(std::size(k_warns) == KILN_WARN_COUNT,
              "k_warns must have one row per kiln_warn_bit_t");
static_assert(indexed_by_code<kiln_warn_bit_t>(k_warns),
              "k_warns must be indexed by kiln_warn_bit_t");

namespace {

/* Seven entries, so a switch costs nothing and -Wswitch-enum makes a new state
 * a compile error until it is named. */
const char *state_name(kiln_state_t state)
{
    switch (state) {
    case KILN_STATE_IDLE:     return "IDLE";
    case KILN_STATE_RUNNING:  return "RUN";
    case KILN_STATE_PAUSED:   return "PAUSE";
    case KILN_STATE_MANUAL:   return "MANUAL";
    case KILN_STATE_AUTOTUNE: return "TUNE";
    case KILN_STATE_COMPLETE: return "DONE";
    case KILN_STATE_FAULT:    return "FAULT";
    case KILN_STATE_COUNT:    break;   /* not a value, only the bound */
    }
    return "?";
}

/* A language outside the table, or one whose column was left empty, falls back
 * to English.  An operator who sees English has a worse day than one who sees
 * German; an operator who sees nothing cannot act at all. */
kiln_lang_t lang_ok(kiln_lang_t lang)
{
    return ((unsigned)lang < (unsigned)KILN_LANG_COUNT) ? lang : KILN_LANG_EN;
}

const char *pick(const char *const by_lang[KILN_LANG_COUNT], kiln_lang_t lang)
{
    const char *v = by_lang[lang_ok(lang)];
    return (v != nullptr && v[0] != '\0') ? v : by_lang[KILN_LANG_EN];
}

} // namespace

const char *kiln_lang_tag(kiln_lang_t lang)
{
    switch (lang_ok(lang)) {
    case KILN_LANG_EN:    return "en";
    case KILN_LANG_DE:    return "de";
    case KILN_LANG_COUNT: break;
    }
    return "en";
}

const char *kiln_fault_label_in(kiln_fault_t code, kiln_lang_t lang)
{
    if ((unsigned)code >= KILN_FAULT_MAX) { return "UNKNOWN"; }
    return pick(k_faults[code].label, lang);
}

const char *kiln_fault_cause_in(kiln_fault_t code, kiln_lang_t lang)
{
    if ((unsigned)code >= KILN_FAULT_MAX) { return "Unknown fault code."; }
    return pick(k_faults[code].cause, lang);
}

const char *kiln_warn_label_in(kiln_warn_bit_t bit, kiln_lang_t lang)
{
    if ((unsigned)bit >= KILN_WARN_COUNT) { return "UNKNOWN"; }
    return pick(k_warns[bit].label, lang);
}

const char *kiln_warn_cause_in(kiln_warn_bit_t bit, kiln_lang_t lang)
{
    if ((unsigned)bit >= KILN_WARN_COUNT) { return "Unknown warning."; }
    return pick(k_warns[bit].cause, lang);
}

/* English, deliberately: diagnostics, the log and the requirement traceability
 * should read the same whoever is looking at them. */
const char *kiln_fault_label(kiln_fault_t code)
{
    return kiln_fault_label_in(code, KILN_LANG_EN);
}

const char *kiln_fault_cause(kiln_fault_t code)
{
    return kiln_fault_cause_in(code, KILN_LANG_EN);
}

const char *kiln_fault_requirement(kiln_fault_t code)
{
    if ((unsigned)code >= KILN_FAULT_MAX || (k_faults[code].req == nullptr)) {
        return "";
    }
    return k_faults[code].req;
}

const char *kiln_warn_label(kiln_warn_bit_t bit)
{
    return kiln_warn_label_in(bit, KILN_LANG_EN);
}

const char *kiln_warn_cause(kiln_warn_bit_t bit)
{
    return kiln_warn_cause_in(bit, KILN_LANG_EN);
}

const char *kiln_state_label(kiln_state_t state)
{
    return state_name(state);
}

/* --- the independent supervisor (SWA-22) --------------------------------- */

const char *kiln_sup_reason_label_in(kiln_sup_reason_t r, kiln_lang_t lang)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= std::size(k_sup_reasons)) {
        return "?";
    }
    return pick(k_sup_reasons[i].label, lang);
}

const char *kiln_sup_reason_label(kiln_sup_reason_t r)
{
    return kiln_sup_reason_label_in(r, KILN_LANG_EN);
}

const char *kiln_sup_reason_cause_in(kiln_sup_reason_t r, kiln_lang_t lang)
{
    const size_t i = static_cast<size_t>(r);
    if (i >= std::size(k_sup_reasons)) {
        return "";
    }
    return pick(k_sup_reasons[i].cause, lang);
}

const char *kiln_sup_reason_cause(kiln_sup_reason_t r)
{
    return kiln_sup_reason_cause_in(r, KILN_LANG_EN);
}
