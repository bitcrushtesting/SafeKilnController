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
/* `panel` is the cause trimmed to what the fault screen can render in full:
 * five rows of 21 characters, action first.  Left empty where the full cause
 * already fits, so there is no second string to drift from the first.  The
 * reason there are two at all is in faults.h; in short, the browser can show a
 * paragraph and a 128x64 panel drops whatever does not fit, which in an
 * instruction is the instruction. */
typedef struct {
    kiln_fault_t code;
    const char  *label[KILN_LANG_COUNT];
    const char  *cause[KILN_LANG_COUNT];
    const char  *panel[KILN_LANG_COUNT];
    const char  *req;
} fault_entry_t;

typedef struct {
    kiln_warn_bit_t code;
    const char     *label[KILN_LANG_COUNT];
    const char     *cause[KILN_LANG_COUNT];
    const char     *panel[KILN_LANG_COUNT];
    const char     *req;
} warn_entry_t;

/* Indexed by kiln_fault_t; order is load-bearing and checked by a unit test. */
constexpr fault_entry_t k_faults[] = {
    { KILN_FAULT_NONE,
        { "OK", "OK" },
        { "No fault",
          "Keine Stoerung" },
        { nullptr, nullptr },
        "" },
    { KILN_FAULT_TC_OPEN,
        { "TC OPEN", "TE OFFEN" },
        { "Kiln thermocouple open circuit. Check the probe and its connections.",
          "Thermoelement des Ofens unterbrochen. Fuehler und Anschluesse pruefen." },
        { nullptr, nullptr },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_SHORT,
        { "TC SHORT", "TE KURZ" },
        { "Kiln thermocouple shorted. Check for damaged insulation.",
          "Thermoelement des Ofens kurzgeschlossen. Auf beschaedigte Isolierung pruefen." },
        { nullptr, nullptr },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_RANGE,
        { "TC RANGE", "TE BEREICH" },
        { "Kiln temperature outside the valid range for this thermocouple type.",
          "Ofentemperatur ausserhalb des gueltigen Bereichs fuer diesen Thermoelementtyp." },
        { nullptr, nullptr },
        "SWR-SAF-04" },
    { KILN_FAULT_CJ,
        { "CJ FAULT", "VERGLEICH" },
        { "Cold junction out of range. The controller itself may be too hot or too cold.",
          "Vergleichsstelle ausserhalb des Bereichs. Der Regler selbst ist moeglicherweise zu heiss oder zu kalt." },
        { nullptr, "Vergleichsstelle ausser Bereich. Regler zu heiss oder zu kalt." },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_COMMS,
        { "TC COMMS", "TE KOMM" },
        { "No response from the thermocouple front end. Check wiring to the MAX31856.",
          "Keine Antwort vom Thermoelement-Messeingang. Verdrahtung zum MAX31856 pruefen." },
        { nullptr, "Keine Antwort vom Messeingang. Verdrahtung pruefen." },
        "SWR-SAF-04" },
    { KILN_FAULT_TC_REVERSED,
        { "TC REVERSED", "TE VERPOLT" },
        { "Temperature fell while heating at high duty: the thermocouple is probably connected with reversed polarity.",
          "Temperatur fiel bei hoher Heizleistung: das Thermoelement ist vermutlich verpolt angeschlossen." },
        { "Temperature fell while heating: the probe is probably reversed.", "Temperatur fiel beim Heizen: Fuehler vermutlich verpolt." },
        "SWR-SAF-05" },
    { KILN_FAULT_TC_STUCK,
        { "TC STUCK", "TE FEST" },
        { "Temperature did not change while heating. The probe reading is stuck.",
          "Temperatur aenderte sich trotz Heizen nicht. Der Messwert haengt fest." },
        { nullptr, nullptr },
        "SWR-SAF-06" },
    { KILN_FAULT_RUNAWAY,
        { "NO HEAT", "KEINE WAERME" },
        { "Heating at high duty produced no temperature rise: failed element, open contactor, open safety chain, or the lid is not closed.",
          "Heizen mit hoher Leistung brachte keinen Temperaturanstieg: defekte Heizwendel, offenes Schuetz, unterbrochene Sicherheitskette oder der Deckel ist nicht geschlossen." },
        { "No rise at high duty. Check elements, contactor, chain, lid.", "Kein Anstieg bei hoher Leistung. Wendel, Schuetz, Deckel pruefen." },
        "SWR-SAF-07" },
    { KILN_FAULT_UNCOMMANDED_HEAT,
        { "SSR SHORT", "SSR KURZ" },
        { "Temperature rising with heating commanded off. The SSR is probably shorted. Isolate the kiln at the wall.",
          "Temperatur steigt, obwohl die Heizung ausgeschaltet ist. Das SSR ist vermutlich durchlegiert. Ofen allpolig vom Netz trennen." },
        { "Heat with the SSR off: it is shorted. Isolate the kiln.", "Temperatur steigt bei Heizung aus: SSR defekt. Netz trennen." },
        "SWR-SAF-08" },
    { KILN_FAULT_OVERTEMP,
        { "OVERTEMP", "UEBERTEMP" },
        { "Kiln temperature exceeded the configured maximum.",
          "Ofentemperatur hat das eingestellte Maximum ueberschritten." },
        { nullptr, nullptr },
        "SWR-SAF-09" },
    { KILN_FAULT_SP_EXCURSION,
        { "OVERSHOOT", "UEBERSCHWING" },
        { "Temperature ran far above setpoint for too long. PID gains may be wrong.",
          "Temperatur lag zu lange deutlich ueber dem Sollwert. Die PID-Parameter sind moeglicherweise falsch." },
        { nullptr, "Zu lange ueber dem Sollwert. PID-Parameter pruefen." },
        "SWR-SAF-10" },
    { KILN_FAULT_CASE_OVERTEMP,
        { "CASE HOT", "GEHAEUSE HEISS" },
        { "Controller enclosure too hot. Improve ventilation or move the controller.",
          "Reglergehaeuse zu heiss. Belueftung verbessern oder den Regler versetzen." },
        { nullptr, nullptr },
        "SWR-SAF-11" },
    { KILN_FAULT_CONTROL_DEADLINE,
        { "CTRL LATE", "REGLER SPAET" },
        { "The control loop missed its deadline. This is a firmware fault.",
          "Der Regelkreis hat seine Frist ueberschritten. Das ist ein Firmwarefehler." },
        { nullptr, nullptr },
        "SWR-SAF-13" },
    { KILN_FAULT_SAFETY_DEADLINE,
        { "SAFE LATE", "SICH SPAET" },
        { "The safety supervisor missed its deadline. This is a firmware fault.",
          "Die Sicherheitsueberwachung hat ihre Frist ueberschritten. Das ist ein Firmwarefehler." },
        { nullptr, "Der Sicherheitszyklus kam zu spaet. Firmwarefehler." },
        "SWR-SAF-13" },
    { KILN_FAULT_WATCHDOG,
        { "WATCHDOG", "WATCHDOG" },
        { "The controller was reset by the watchdog during a firing.",
          "Der Regler wurde waehrend eines Brandes vom Watchdog zurueckgesetzt." },
        { nullptr, nullptr },
        "SWR-SAF-14" },
    { KILN_FAULT_CASE_TC,
        { "CASE TC", "GEH TE" },
        { "Enclosure thermocouple fault. Enclosure temperature cannot be supervised.",
          "Stoerung am Gehaeuse-Thermoelement. Die Gehaeusetemperatur kann nicht ueberwacht werden." },
        { nullptr, "Stoerung am Gehaeusefuehler. Gehaeusewert fehlt." },
        "SWR-SAF-04" },
    { KILN_FAULT_OPERATOR_ABORT,
        { "ABORTED", "ABGEBROCHEN" },
        { "Firing aborted by the operator.",
          "Brand vom Bediener abgebrochen." },
        { nullptr, nullptr },
        "SWR-RUN-04" },
    { KILN_FAULT_TUNE_NO_CONVERGE,
        { "TUNE FAIL", "TUNING FEHL" },
        { "Automatic tuning did not find a stable oscillation. Previous gains kept.",
          "Die automatische Abstimmung fand keine stabile Schwingung. Die bisherigen Parameter bleiben erhalten." },
        { nullptr, "Keine stabile Schwingung. Alte PID-Parameter bleiben." },
        "SWR-TUN-07" },
    { KILN_FAULT_RECOVERY_REFUSED,
        { "NO RESUME", "KEIN RESTART" },
        { "Power was lost during a firing and the kiln had cooled too far to resume safely.",
          "Waehrend des Brandes fiel die Spannung aus und der Ofen war zu weit abgekuehlt, um sicher fortzusetzen." },
        { nullptr, "Stromausfall im Brand; Ofen zu kalt zum Fortsetzen." },
        "SWR-RUN-08" },
    { KILN_FAULT_CONFIG_STORAGE,
        { "STORAGE", "SPEICHER" },
        { "Configuration could not be read or written. Defaults are in use.",
          "Die Konfiguration konnte nicht gelesen oder geschrieben werden. Es gelten die Standardwerte." },
        { nullptr, nullptr },
        "SWR-CFG-05" },
    { KILN_FAULT_UNCOMMANDED_CURRENT,
        { "RELAY ON", "RELAIS EIN" },
        { "Heater current is flowing with heating commanded off. A relay is stuck on.",
          "Es fliesst Heizstrom, obwohl die Heizung ausgeschaltet ist. Ein Schaltglied haengt." },
        { nullptr, "Heizstrom bei Heizung aus. Ein Schaltglied haengt." },
        "SWR-SAF-25" },
    { KILN_FAULT_CONTACTOR_WELDED,
        { "WELDED", "VERSCHWEISST" },
        { "Current continued after the contactor was commanded open: the contactor is welded shut. The controller can no longer interrupt the heaters. ISOLATE THE KILN AT ITS SUPPLY NOW.",
          "Der Strom floss weiter, nachdem das Schuetz geoeffnet werden sollte: das Schuetz ist verschweisst. Der Regler kann die Heizung nicht mehr abschalten. OFEN SOFORT ALLPOLIG VOM NETZ TRENNEN." },
        { "ISOLATE THE KILN NOW. The contactor is welded: heat cannot stop.", "OFEN SOFORT VOM NETZ TRENNEN. Schuetz verschweisst." },
        "SWR-SAF-27" },
    { KILN_FAULT_NO_HEATER_CURRENT,
        { "NO CURRENT", "KEIN STROM" },
        { "No heater current with heating commanded on: failed SSR, open contactor, blown heater fuse, open safety chain or open elements.",
          "Kein Heizstrom bei eingeschalteter Heizung: defektes SSR, offenes Schuetz, durchgebrannte Heizungssicherung, unterbrochene Sicherheitskette oder offene Heizwendeln." },
        { "No current with heat on. Check SSR, contactor, fuse, chain.", "Kein Strom bei Heizung an. SSR, Schuetz, Sicherung pruefen." },
        "SWR-SAF-26" },
    { KILN_FAULT_CURRENT_DEVIATION,
        { "CURR DEV", "STROM ABW" },
        { "Heater current differs sharply from this run's reference. An element group has probably failed.",
          "Der Heizstrom weicht stark vom Referenzwert dieses Brandes ab. Vermutlich ist eine Heizgruppe ausgefallen." },
        { "Current far off this run's reference. An element group failed.", "Strom weicht stark ab. Heizgruppe ausgefallen." },
        "SWR-SAF-28" },
    { KILN_FAULT_OVERCURRENT,
        { "OVERCURRENT", "UEBERSTROM" },
        { "Heater current above the configured maximum: shorted element, wrong wiring or a failed SSR.",
          "Heizstrom ueber dem eingestellten Maximum: Windungsschluss, falsche Verdrahtung oder ein defektes SSR." },
        { "Current above the set maximum: shorted element or wiring.", "Strom ueber Maximum: Schluss oder falsche Verdrahtung." },
        "SWR-SAF-29" },
    { KILN_FAULT_CT_FAULT,
        { "CT FAULT", "WANDLER" },
        { "No signal from the current transformer. Check that it is fitted around one heater conductor and plugged in.",
          "Kein Signal vom Stromwandler. Pruefen, ob er um einen Heizleiter gelegt und angeschlossen ist." },
        { "No signal from the CT. Is it round a heater wire and plugged in?", "Kein Signal vom Wandler. Um Heizleiter gelegt und gesteckt?" },
        "SWR-CUR-11" },
    { KILN_FAULT_DOOR_OPEN,
        { "DOOR OPEN", "TUER OFFEN" },
        { "The kiln door was opened during a firing. Heating stopped immediately. Close the door and acknowledge to continue.",
          "Die Ofentuer wurde waehrend eines Brandes geoeffnet. Die Heizung wurde sofort abgeschaltet. Tuer schliessen und quittieren, um fortzufahren." },
        { "Door opened during a firing. Close it and acknowledge.", "Tuer im Brand geoeffnet. Schliessen und quittieren." },
        "SWR-SAF-31" },
};

constexpr warn_entry_t k_warns[] = {
    { KILN_WARN_INSULATION,
        { "INSULATION", "ISOLIERUNG" },
        { "This firing needed noticeably more energy than usual. Insulation or elements may be degrading.",
          "Dieser Brand benoetigte spuerbar mehr Energie als ueblich. Isolierung oder Heizwendeln koennten nachlassen." },
        { "This firing used more energy than usual. Insulation may age.", "Mehr Energie als ueblich. Isolierung laesst nach." },
        "SWR-SAF-12" },
    { KILN_WARN_HOLDBACK,
        { "HOLDBACK", "HALTEN" },
        { "The kiln is behind its curve, so the schedule is being held back.",
          "Der Ofen liegt hinter seiner Kurve, daher wird der Zeitplan zurueckgehalten." },
        { nullptr, nullptr },
        "SWR-CTL-11" },
    { KILN_WARN_LOG_UNAVAIL,
        { "NO LOG", "KEIN LOG" },
        { "Temperature logging is unavailable. The firing continues.",
          "Die Temperaturaufzeichnung ist nicht verfuegbar. Der Brand laeuft weiter." },
        { nullptr, "Messwertaufzeichnung fehlt. Der Brand laeuft weiter." },
        "SWR-LOG-14" },
    { KILN_WARN_DISPLAY_UNAVAIL,
        { "NO DISPLAY", "KEIN DISPLAY" },
        { "The display is not responding. The firing continues.",
          "Das Display antwortet nicht. Der Brand laeuft weiter." },
        { nullptr, nullptr },
        "SWR-HMI-14" },
    { KILN_WARN_TIME_UNSYNCED,
        { "NO TIME", "KEINE ZEIT" },
        { "Clock not synchronised; log timestamps are relative only.",
          "Uhr nicht synchronisiert; Zeitstempel im Log sind nur relativ." },
        { nullptr, nullptr },
        "SWR-LOG-12" },
    { KILN_WARN_WIFI_DOWN,
        { "NO WIFI", "KEIN WLAN" },
        { "WiFi is down. The firing is unaffected.",
          "WLAN ist nicht verbunden. Der Brand ist davon nicht betroffen." },
        { nullptr, nullptr },
        "SWR-NET-07" },
    { KILN_WARN_DUTY_SATURATED,
        { "FULL POWER", "VOLLLAST" },
        { "Heating has been at full power for a long time; the kiln may not keep up.",
          "Die Heizung laeuft seit Laengerem auf Volllast; der Ofen kommt moeglicherweise nicht nach." },
        { nullptr, "Lange auf Volllast; der Ofen kommt nicht nach." },
        "SWR-CTL-15" },
    { KILN_WARN_GAINS_UNTUNED,
        { "UNTUNED", "UNGETUNT" },
        { "PID gains are factory defaults. Run automatic tuning for this kiln.",
          "Die PID-Parameter sind Werkseinstellungen. Automatische Abstimmung fuer diesen Ofen durchfuehren." },
        { nullptr, "PID ist Werkseinstellung. Abstimmung empfohlen." },
        "SWR-TUN-11" },
    { KILN_WARN_RELAY_WEAR,
        { "RELAY WEAR", "RELAIS VERSCHL" },
        { "A relay has reached its configured switching-operation life limit. Plan to replace it.",
          "Ein Schaltglied hat seine eingestellte Schaltspielzahl erreicht. Austausch einplanen." },
        { nullptr, nullptr },
        "SWR-SAF-30" },
    { KILN_WARN_RELAY_SUSPECT,
        { "RELAY?", "RELAIS?" },
        { "Heater current has intermittently disagreed with the commanded state. A relay may be starting to fail.",
          "Der Heizstrom wich zeitweise vom Schaltzustand ab. Ein Schaltglied koennte ausfallen." },
        { "Current sometimes disagrees with the command. A relay may fail.",
          "Strom passt zeitweise nicht zum Befehl. Relais kann ausfallen." },
        "SWR-SAF-30" },
    { KILN_WARN_CURRENT_OFF,
        { "NO CT", "KEIN WANDLER" },
        { "Current monitoring is disabled. Relay faults can only be inferred from temperature, which is far slower.",
          "Die Stromueberwachung ist abgeschaltet. Relaisfehler lassen sich dann nur ueber die Temperatur erkennen, was weit langsamer ist." },
        { "Current monitoring is off. Relay faults show only in heat.", "Stromueberwachung aus. Relaisfehler nur ueber Temperatur." },
        "SWR-CUR-12" },
    { KILN_WARN_CURRENT_DEV,
        { "CURR LOW", "STROM NIEDRIG" },
        { "Heater current is drifting from this run's reference. Elements may be ageing.",
          "Der Heizstrom driftet vom Referenzwert dieses Brandes ab. Die Heizwendeln koennten altern." },
        { nullptr, "Strom weicht von der Referenz ab. Wendeln altern." },
        "SWR-SAF-28" },
    { KILN_WARN_DOOR_OFF,
        { "NO DOOR SW", "KEIN TUERSCH" },
        { "No door interlock is fitted. Opening the door during a firing will not stop the heater.",
          "Es ist kein Tuerkontakt eingebaut. Ein Oeffnen der Tuer waehrend eines Brandes schaltet die Heizung nicht ab." },
        { "No door interlock. Opening the door will not stop the heater.", "Kein Tuerkontakt. Oeffnen stoppt die Heizung nicht." },
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
    const char *panel[KILN_LANG_COUNT];
} k_sup_reasons[] = {
    { KILN_SUP_OK,
        { "OK", "OK" },
        { "The independent supervisor is permitting heat.",
          "Der unabhaengige Waechter gibt die Heizung frei." },
        { nullptr, nullptr } },
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
          "Bedienfeld druecken." },
        { "Supervisor stopped the kiln over its fixed limit. Press clear.", "Waechter hat ueber fester Grenze gestoppt. Quittieren." } },
    { KILN_SUP_TC_FAULT,
        { "SUPERVISOR TC FAULT", "WAECHTER FUEHLERFEHLER" },
        { "The independent supervisor's thermocouple front end reported a "
          "fault. Check the chamber probe and its wiring, then press the "
          "supervisor's clear button on the panel.",
          "Die Fuehlerelektronik des unabhaengigen Waechters meldet einen "
          "Fehler. Kammerfuehler und Verdrahtung pruefen, dann Quittiertaste "
          "des Waechters am Bedienfeld druecken." },
        { "Supervisor probe fault. Check the chamber probe, then clear.", "Fuehlerfehler am Waechter. Fuehler pruefen, quittieren." } },
    { KILN_SUP_SENSOR_STALE,
        { "SUPERVISOR NO READING", "WAECHTER KEIN MESSWERT" },
        { "The independent supervisor was reading the chamber and stopped "
          "getting a value. Check the probe and its wiring, then press the "
          "supervisor's clear button on the panel.",
          "Der unabhaengige Waechter hat die Kammer gemessen und erhaelt "
          "keinen Wert mehr. Fuehler und Verdrahtung pruefen, dann "
          "Quittiertaste des Waechters am Bedienfeld druecken." },
        { "Supervisor lost the chamber reading. Check the probe, then clear.", "Waechter ohne Messwert. Fuehler pruefen, quittieren." } },
    { KILN_SUP_SELF_TEST,
        { "SUPERVISOR SELF-TEST", "WAECHTER SELBSTTEST" },
        { "The independent supervisor could not configure its thermocouple "
          "front end at start-up and will not permit heat. This is not "
          "clearable by the operator; the supervisor needs servicing.",
          "Der unabhaengige Waechter konnte seine Fuehlerelektronik beim Start "
          "nicht konfigurieren und gibt die Heizung nicht frei. Nicht durch "
          "den Bediener quittierbar; der Waechter muss instand gesetzt "
          "werden." },
        { "Supervisor could not set up its front end. It needs servicing.", "Waechter nicht einsatzbereit. Service noetig." } },
    { KILN_SUP_LINK_DEAD,
        { "NO SUPERVISOR", "KEIN WAECHTER" },
        { "Nothing is arriving from the independent supervisor, so there is no "
          "chamber temperature and no heat. This is the absence of the "
          "backstop rather than the backstop acting: check that the supervisor "
          "is powered and its link is connected.",
          "Vom unabhaengigen Waechter kommt nichts an, daher gibt es keine "
          "Kammertemperatur und keine Heizung. Das ist das Fehlen der "
          "Rueckfallebene, nicht ihr Ansprechen: pruefen, ob der Waechter "
          "versorgt und die Verbindung angeschlossen ist." },
        { "Nothing from the supervisor: no temperature, no heat. Check it.", "Keine Daten vom Waechter: keine Hitze. Strom und Kabel pruefen." } },
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
          "Bedienfeld druecken." },
        { "The supervisor's two probes disagree. Check both, then clear.", "Die zwei Fuehler weichen ab. Beide pruefen, quittieren." } },
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
          "wieder benutzt wird." },
        { "ISOLATE THE KILN. The coil stayed on after permission was cut.", "OFEN VOM NETZ TRENNEN. Spule blieb trotz Sperre an." } },
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

/* The panel form, falling back to the full cause where none is needed.  The
 * fallback is the reason an entry that fits carries no second string: one
 * sentence that is known to fit is better than two that can disagree. */
const char *kiln_fault_panel_in(kiln_fault_t code, kiln_lang_t lang)
{
    if ((unsigned)code >= KILN_FAULT_MAX) { return "Unknown fault code."; }
    const char *s = pick(k_faults[code].panel, lang);
    return (s != nullptr && s[0] != '\0') ? s : pick(k_faults[code].cause, lang);
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

const char *kiln_warn_panel_in(kiln_warn_bit_t bit, kiln_lang_t lang)
{
    if ((unsigned)bit >= KILN_WARN_COUNT) { return "Unknown warning."; }
    const char *s = pick(k_warns[bit].panel, lang);
    return (s != nullptr && s[0] != '\0') ? s : pick(k_warns[bit].cause, lang);
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

const char *kiln_sup_reason_panel_in(kiln_sup_reason_t r, kiln_lang_t lang)
{
    if ((unsigned)r >= KILN_SUP_REASON_COUNT) { return "Unknown supervisor state."; }
    const char *s = pick(k_sup_reasons[r].panel, lang);
    return (s != nullptr && s[0] != '\0') ? s : pick(k_sup_reasons[r].cause, lang);
}

const char *kiln_sup_reason_cause(kiln_sup_reason_t r)
{
    return kiln_sup_reason_cause_in(r, KILN_LANG_EN);
}
