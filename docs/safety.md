<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# KilnControl, Safety Concept

| | |
|---|---|
| **Document** | Safety Concept |
| **Project** | KilnControl, PID kiln controller |
| **Version** | 0.1 (draft) |
| **Date** | 2026-10-05 |
| **Status** | For review |
| **Derives from** | [`requirements.sdoc`](requirements.sdoc) v0.1, [`architecture.md`](architecture.md) v0.1 |
| **License** | GPL-3.0-or-later |

---

> **KilnControl is not a safety-certified device.** This document explains the
> reasoning behind its protective measures; it is not a declaration of
> conformity, a functional-safety assessment to IEC 61508 or ISO 13849, and it
> does not assign a SIL or a performance level. An **independent hardware
> over-temperature cutout is required** in every installation
> ([HR-13](requirements.sdoc)), mains wiring must
> be carried out by a competent person in accordance with local regulation [R6],
> and the kiln must not be fired unattended
> ([ASM-06](requirements.sdoc)). See [§9](#9-limitations-and-obligations).

## 1. Purpose and scope

[`requirements.sdoc` §5](requirements.sdoc) states *what* the
system must detect and do. [`architecture.md` §8](architecture.md#8-safety-subsystem)
states *how* the software is built to do it. This document supplies the layer
between them: the hazards being defended against, the safety goals derived from
those hazards, the protection layers that meet each goal, the independence
claimed between those layers, and, explicitly, the risk that remains.

It exists so that a reviewer can answer three questions without reading the
code:

1. What can go wrong, and how badly?
2. For each of those, what stops it, and what stops it if the first thing fails?
3. What is left over, and who is responsible for it?

Identifiers introduced here extend the scheme of
[requirements §1.4](requirements.sdoc):

| Prefix | Meaning |
|---|---|
| `HZ` | Hazard |
| `SG` | Safety goal |
| `RR` | Residual risk |

Requirement identifiers (`SR-07`, `HR-13`, `NFR-04`, `ASM-06`) are cited as
defined in the requirements specification and are not restated here.

## 2. System boundary

The *equipment under control* is an electric resistance kiln: a multi-kilowatt
mains heater whose chamber reaches up to 1350 °C and whose outer surfaces,
wiring and surroundings are at risk when control is lost.

```mermaid
flowchart TB
    subgraph OUT["Outside the boundary, installation and operator"]
        SUP["Mains supply, isolator,<br/>fuse or MCB"]
        CUT["Independent over-temperature<br/>cutout (HR-13)"]
        OP["Attending operator (ASM-06)"]
        KILN["Kiln body, elements,<br/>lid interlock, wiring"]
    end
    subgraph IN["Inside the boundary, KilnControl"]
        MCU["ESP32-S3 firmware:<br/>control + safety supervisor"]
        CP["Charge pump → contactor coil"]
        CON["Safety contactor"]
        SSR["Zero-cross SSR"]
        TC["2 × MAX31856<br/>chamber + enclosure"]
        CT["Current transformer"]
        BUZ["Buzzer"]
    end
    SUP --> CUT --> CON --> SSR --> KILN
    MCU --> CP --> CON
    MCU --> SSR
    TC --> MCU
    CT --> MCU
    MCU --> BUZ --> OP
    OP -.->|"acknowledge, abort,<br/>isolate at supply"| MCU
```

**Inside the boundary.** The firmware, the two switching devices and their
drive circuits, the two thermocouple front ends, the current transformer and
its conditioning, and the annunciation.

**Outside the boundary, but relied upon.** The supply protection, the
independent cutout, the kiln's own construction and the operator. Each is the
subject of an assumption in
[requirements §9](requirements.sdoc), and every assumption that is
load-bearing for a safety goal is identified as such in [§6](#6-independence)
and [§9](#9-limitations-and-obligations). An assumption that is wrong in the
field is a defeated protection layer, which is why they are listed rather than
left implicit.

## 3. Hazard analysis

Hazards are rated qualitatively. *Severity* is the worst credible outcome, not
the typical one. *Detectability* is how readily the system, not the operator , 
can observe the condition. No probability figures are given: the project has no
field population from which to derive them, and inventing them would make the
analysis look more rigorous than it is.

| ID | Hazard | Credible causes | Worst credible consequence | Severity |
|---|---|---|---|---|
| **HZ-01** | **Uncontrolled heating**: power reaching the elements that the controller did not command and cannot withdraw | Shorted SSR; welded contactor; firmware fault holding the output on; a static heat-enable level surviving a crash | Chamber past its rating, kiln wiring and surrounding structure ignite; building fire | Catastrophic |
| **HZ-02** | **Commanded over-temperature**: the controller heats to a temperature it should not | Setpoint or program error; configured maximum set above the kiln's rating; unit confusion | Elements and brickwork destroyed; load destroyed; chamber past the rating of the kiln's own wiring | Severe |
| **HZ-03** | **Heating against a false low reading**: control is nominally correct but the measurement is wrong | Reversed thermocouple; stuck reading; drifted or mis-calibrated channel; front end returning a plausible but stale value | As HZ-01: full duty indefinitely while the display reads a comfortable number | Catastrophic |
| **HZ-04** | **Loss of control with heat enabled** | Task hang, deadlock, priority inversion, unbounded blocking in control or safety; panic; watchdog expiry | Last commanded duty persists with nothing supervising it | Catastrophic |
| **HZ-05** | **Enclosure over-temperature** | Insulation degradation; contactor or SSR dissipation; fan or ventilation blocked; ambient | Controller electronics fail, which is HZ-04, and wiring insulation softens at the one place where mains and logic are close together | Severe |
| **HZ-06** | **Heater over-current** | Shorted element; mis-wiring; SSR failing short with a lower-impedance path | Conductor and connector overheating upstream of any thermal sensor; fire at the terminal block | Severe |
| **HZ-07** | **Electric shock or arc exposure** | Mains wiring faults; loss of isolation between the heater circuit and the controller; CT secondary opened while carrying primary current | Electrocution; arc flash at the terminals | Catastrophic |
| **HZ-08** | **Unsupervised re-energisation**: the kiln heats again after a fault or a power loss without a person deciding that it should | Self-clearing fault logic; recovery policy resuming a run whose cause of interruption is unknown; fault state not surviving reset | HZ-01 or HZ-02 with nobody present and nobody informed | Catastrophic |
| **HZ-09** | **Undetected failure to heat** | Open element; open contactor; open safety chain; blown heater fuse; SSR failed open | Not itself a thermal hazard, but it masks a broken safety chain and ruins the load; an operator who sees only "cold" may bypass the chain to find out why | Moderate |
| **HZ-10** | **Protection defeated by nuisance tripping** | A detection rule tuned so tightly that it stops healthy firings | The operator widens the threshold to its limit, disables current monitoring, or bypasses the cutout. A rule that stops a healthy firing is worse than no rule, because it gets switched off | Severe |
| **HZ-13** | **Chamber opened during a firing**: the door is opened while the elements are live | Operator opening a hot kiln to look at the load; a lid left unlatched; an interlock not fitted | Exposure to a live element at mains potential, and to a 1000 °C chamber at arm's length; thermal shock to the load | Catastrophic |
| **HZ-11** | **Hot surfaces and residual heat** | Normal operation; a kiln at temperature after a fault or a power loss | Burns on contact; ignition of material stored against the kiln | Severe |
| **HZ-12** | **Remote command causing a hazardous state** | Unauthenticated or hostile access to the web interface over the local network | A firing started, altered or a fault acknowledged by someone not at the kiln. **Largely closed by `FR-WEB-26`**: no network route can put heat into the kiln, write configuration or clear a fault. What remains is program *authoring*, an attacker can alter a curve the operator then starts at the kiln, bounded by `SR-23`'s ceiling. | Severe → **Moderate** |

HZ-07 and HZ-11 are predominantly installation and operating hazards: the
firmware can reduce neither by much, and both are discharged through
construction requirements and documentation rather than through detection. They
are listed because omitting them from a safety concept would misrepresent where
the risk in a kiln actually lies.

## 4. Safety goals

Each goal is the inversion of one or more hazards into something the design can
be held to.

| ID | Safety goal | Addresses | Realised by |
|---|---|---|---|
| **SG-01** | No single failure of software, MCU, sensor or switching device shall result in heating; every such failure shall result in heating being removed | HZ-01, HZ-03, HZ-04 | `SR-01`, and the whole of [§5](#5-the-protection-layers) |
| **SG-02** | Heating shall exist only while software is *actively and continuously* asserting it; no static condition shall be able to hold it | HZ-01, HZ-04 | `SR-02`, `HR-07`, `HR-08`, `AD-05` |
| **SG-03** | There shall be two series interrupting devices under independent control, so that one failing closed does not remove the ability to interrupt | HZ-01 | `SR-03`, `HR-06`, `HR-07` |
| **SG-04** | A measurement that is wrong shall be detected as wrong rather than acted upon | HZ-03 | `SR-04`–`SR-06`, `FR-ACQ-12` |
| **SG-05** | A failure of the switching devices or the elements shall be detected electrically, within seconds, independently of its thermal effect | HZ-01, HZ-06, HZ-09 | `SR-25`–`SR-30`, `FR-CUR`, `NFR-27` |
| **SG-06** | Temperature shall be bounded by configuration and by an absolute ceiling that no configuration can raise | HZ-02 | `SR-09`, `SR-23`, `SR-10` |
| **SG-07** | A detected fault shall remove heating, be annunciated, persist across power loss, and require a deliberate human act to clear, which shall be refused while the cause is still present | HZ-08 | `SR-16`–`SR-20`, `FR-RUN-08` |
| **SG-08** | The controller's own operating environment shall be kept inside its limits, and exceeded limits treated as a fault | HZ-05 | `SR-11`, `SR-12` |
| **SG-09** | Detection shall remain effective when any single detection channel is absent or has failed | HZ-01, HZ-03, HZ-09 | Defence in depth, [§6](#6-independence) |
| **SG-10** | Every threshold shall be bounded, no detection shall be disableable, and rules shall be specified so that a healthy firing does not trip them | HZ-10 | `SR-22`, `SR-23`, and the confirmation windows of [§7.3](#73-why-two-rules-carry-a-confirmation-window) |
| **SG-11** | Heating shall be de-energised during reset, boot, firmware update and any transition through an undefined software state | HZ-01, HZ-04, HZ-08 | `SR-21`, `NFR-09`, `NFR-15`, `FR-UPD-02` |
| **SG-13** | Opening the chamber shall remove power from the elements immediately, through hardware as well as software | HZ-13 | `SR-31`, `HR-21` |
| **SG-12** | No control action that can heat the kiln shall be reachable over the network at all; what remains reachable shall be authenticated | HZ-12 | **`FR-WEB-26`**, `NFR-19`, `NFR-20`, `ASM-05` |

## 5. The protection layers

The concept is layered, and the layers are ordered by how little they depend on
software being correct. Nothing below is a substitute for anything above it.

```mermaid
flowchart TB
    L1["**L1: Control**<br/>setpoint clamped to the configured max,<br/>configured max clamped to 1350 °C (SR-23)"]
    L2["**L2: Safety supervisor**<br/>18 rules, 100 ms cycle, sole heat authority (AD-04)<br/>thermal channel + current channel"]
    L3["**L3: Heat authority hardware**<br/>charge pump: a square wave, not a level (AD-05)<br/>contactor in series with the SSR (SR-03)"]
    L4["**L4: Reset to safe**<br/>task + RTC watchdog, brownout detector,<br/>output pull-downs (HR-08), latched fault in NVS"]
    L5["**L5: Independent hardware cutout**<br/>own sensor, own contacts, outside this project (HR-13)"]
    L6["**L6: Installation and operator**<br/>supply protection, competent installation,<br/>attendance (ASM-04, ASM-06)"]
    L1 --> L2 --> L3 --> L4 --> L5 --> L6
    style L1 fill:#e8f0fe,stroke:#4a76c4
    style L2 fill:#e8f0fe,stroke:#4a76c4
    style L3 fill:#fef0e0,stroke:#c48a4a
    style L4 fill:#fef0e0,stroke:#c48a4a
    style L5 fill:#e9f6ea,stroke:#4aa052
    style L6 fill:#e9f6ea,stroke:#4aa052
```

### 5.1 L1, Control

The control path can only *request* a duty. It holds no authority and is not a
protection layer in its own right; it is listed because its clamping behaviour
removes the commonest route to HZ-02. Every setpoint, program target and tuning
setpoint is clamped to the configured maximum chamber temperature, and the
configured maximum is itself bounded by a compile-time ceiling of 1350 °C that
no configuration value can raise (`SR-23`).

### 5.2 L2, The safety supervisor

A separate task at higher priority than control, pinned to the core that
networking cannot reach (`AD-15`), running at 10 Hz against a requirement of
4 Hz (`NFR-03`). It is the **only** holder of heat authority: in the whole
firmware there is exactly one assignment that grants it, in the safety cycle of
`kiln_app`, and the control, HMI and web paths cannot reach it.

Each rule is a pure function of an immutable input snapshot plus its own timer
state, no globals, injected time (`AD-02`, `AD-03`), which is what allows a
15-minute runaway timer and a 168-hour firing to be exercised in milliseconds
on a development host. The rules and their thresholds are tabulated in
[architecture §8.2](architecture.md#82-rule-table); they are not duplicated
here, because a threshold that appears in two documents eventually disagrees
with itself.

Two detection channels run inside this layer, and their separation is the point
of [§6](#6-independence):

- **Thermal** (`SR-04`–`SR-13`), reasoning about temperature, its rate of
  change and the commanded duty, via the MAX31856 channels.
- **Electrical** (`SR-25`–`SR-30`), reasoning about current measured by the CT,
  gated to the commanded output state (`AD-17`) so that the measurement is a
  statement about the *relay*, not an average over a mostly-off window.

### 5.3 L3, The heat authority hardware

This is the layer that does not depend on the firmware being correct, only on
it running, and it is the central safety property of the design.

Heat enable is **a software-generated square wave into a hardware charge pump**
(`AD-05`, `HR-07`), not a GPIO level. The supervisor toggles it once per cycle;
stop toggling, crash, hang, deadlock, deadline miss, panic, power loss, and
the coil de-energises within about a second with no code involved. This is what
discharges SG-02, and it is why `SR-14`'s watchdog requirement costs nothing
extra: a watchdog expiry removes heat by the same mechanism as any other way of
failing to run.

> **The toggle must never be delegated to a hardware PWM peripheral**, and the
> circuit must never be "simplified" to a static GPIO. Either change leaves the
> board working and silently destroys the property. The schematic marks the
> circuit as safety-critical, and the HIL suite verifies contactor release by
> halting the safety task.

**Four independent things now break the coil, three of them without firmware.**
The coil circuit is a series chain, and any element opening drops the contactor:

| Element | Opens when | Firmware involved? |
|---|---|---|
| Lid switch contacts (`HR-21`) | the door opens | **No.** Mechanical, in the coil circuit. |
| `Q6`, gated by the chamber `FAULT` output (`HR-24`) | the chamber front end reports a fault | No, once configured. See the limit below. |
| `Q5`, gated by the enclosure `FAULT` output (`HR-24`) | the enclosure front end reports a fault | As above. |
| `Q3`, gated by the charge pump (`AD-05`) | the supervisor stops toggling | Only in that it must keep running. |

The freewheel diode sits **across the coil**, on the kiln side of all four
(`HR-25`), so opening any of them leaves the coil current somewhere to decay
and does not put the transient across the opening contacts.

The limit of the two `FAULT` elements is worth stating rather than discovering:
the outputs are open-drain, so an **unpowered or absent** front end leaves its
transistor on and the path closed, and the MAX31856 detects an open circuit
only after its fault mask has been configured. They interrupt on faults the
device actively *reports*. A dead or unconfigured front end is covered by
`SR-04` in firmware (fault 5, front-end communication failure), not here. The
lid switch carries no such caveat: it is a contact in the circuit.

In series with the charge-pump-driven contactor sits the SSR, modulating under
the supervisor's duty command (`SR-03`). Two devices, two drive circuits, two
failure modes. All heater and contactor control outputs carry external
pull-downs to the de-energised state and avoid strapping pins and pins that
glitch during reset (`HR-08`), so the safe state holds through the window before
the firmware's first instruction (`SG-11`).

### 5.4 L4, Reset to the safe state

The hardware watchdog and a per-task watchdog cover the control and safety tasks
(`SR-14`); the brownout detector is enabled (`SR-15`). Any of them firing leaves
heating de-energised through L3 and records the reset cause in non-volatile
storage (`NFR-15`). The boot path brings outputs to the safe state first, starts
the supervisor before networking, the HMI or the web server, and reaches a state
where temperature is measured and heating is safely off within 3 s of reset
(`NFR-09`), so there is no window in which heating could be enabled without
supervision.

### 5.5 L5, The independent hardware cutout

Outside this project entirely: its own sensor, its own contacts, in the safety
chain ahead of everything the controller drives (`HR-13`). It is **mandatory**,
not advisory, and it is the only layer that is unaffected by a design error
anywhere in L1–L4. The project does not propose to replace it, and
[requirements §12](requirements.sdoc) places doing so explicitly
out of scope.

### 5.6 L6, Installation and operator

Supply protection and competent mains installation (`ASM-04`), and operator
attendance during firing (`ASM-06`), as kiln manufacturers themselves require.
Annunciation exists to make this layer effective: an on-board buzzer sounds on
any fault, with a pattern audibly distinguishable from program completion
(`SR-20`, `HR-09`), and it sounds *after* the fault is in non-volatile storage,
because `SR-17` is explicit that an immediate power loss must not lose it and
the alarm is the moment the operator starts reacting.

## 6. Independence

Layering is only worth what the independence between the layers is worth. This
section states where the claim is genuine and where it is not.

| Pair | Independent? | Shared element, what defeats both |
|---|---|---|
| L1 control vs. L2 thermal rules | **No** | The same chamber thermocouple. A plausible-but-wrong reading (HZ-03) misleads both. This is the entire reason `SR-04`–`SR-06` exist: they do not measure temperature, they interrogate the *measurement*, fault bits, polarity, and whether the number moves at all. |
| L2 thermal rules vs. L2 current rules | **Yes, physically** | Different sensor (CT vs. thermocouple), different quantity (amps vs. degrees), different front end, different failure modes. They share the supervisor task and the MCU. |
| L2 vs. L3 | **Yes, for the failure class that matters** | L3 does not depend on L2 being *correct*, only on it *running*. A logic error in a rule does not stop the toggle; a hang, crash or overrun stops it immediately. The converse is also true: L3 cannot detect anything, so a subtly wrong rule is L2's problem alone. |
| L3 SSR vs. L3 contactor | **Yes** | Separate devices on separate outputs. A shorted SSR leaves the contactor able to interrupt; this is what `SR-27`'s weld discrimination tests, and what makes "SSR shorted" a recoverable fault and "contactor welded" not. |
| L2+L3 vs. L4 watchdogs | **Partly** | Same MCU and same power rail. A supply fault defeats all three, and is then safe by construction, since every one of them fails towards de-energised. |
| L1–L4 vs. L5 cutout | **Yes, fully** | Nothing is shared: separate sensor, separate contacts, separate failure modes. This is why L5 is mandatory and why no amount of software may be offered as a substitute. |

**Defence in depth, stated as a rule rather than an aspiration.** The current
rules are the *primary* detection of a failed relay, contactor or element,
because they are fast and unambiguous. The thermal rules are retained
**unchanged** as a backstop, because they use a different sensor and a different
physical principle and so still cover the kiln whose current monitoring is
disabled, whose transformer has failed or was never fitted (`FR-CUR-12`), or
whose fault is on an unmonitored phase (`ASM-10`). **Neither may be removed on
the grounds that the other exists**: this is a standing constraint on future
changes, not a description of the present state.

## 7. Detection and reaction

### 7.1 Coverage by failure mode

Read this table as: for each way the equipment can fail dangerously, which
independent things notice, and how fast.

| Failure | Primary detection | Independent backstop | Last line | Codes |
|---|---|---|---|---|
| SSR shorted (conducting uncommanded) | `SR-25`: current in an off-window, **≤ 1 s** (`NFR-27`) | `SR-08`: +5 °C over 3 min at zero duty; see the caveat in [§7.3](#73-why-two-rules-carry-a-confirmation-window) | Contactor opens; L5 cutout | 21 → 9 |
| Contactor welded | `SR-27`: discrimination after `SR-25`, verdict within a further 3 s |, | **L5 cutout and the operator only** | 22 |
| Element open or partially open | `SR-26` (no conduction current), `SR-28` (deviation from the run reference) | `SR-07`: runaway: high duty, no rise |, | 23, 24, warning 112 |
| Shorted element / over-current | `SR-29`: > 120 % of nominal | Supply fuse or MCB (outside the boundary) | L6 | 25 |
| Thermocouple open, shorted, out of range, cold-junction fault, comms failure | MAX31856 fault bits + `SR-04`, after the `FR-ACQ-12` grace period |, | L5 cutout | 1–5, 16 |
| Thermocouple reversed | `SR-05`: a *persistent* fall while duty is high | `SR-07` | L5 cutout | 6 |
| Sensor stuck at a plausible value | `SR-06`: < 2 °C of movement over 10 min at > 50 % duty | `SR-07`; `SR-12` on energy-to-temperature | L5 cutout | 7 |
| Heating with no temperature response (open lid, open safety chain, dead element) | `SR-26`: electrically, in seconds | `SR-07`: thermally, in 15 min |, | 23, 8 |
| Door opened during a firing | `SR-31`: heat off and contactor dropped on the first open sample, latched after 0.2 s | **`HR-21`'s series wiring into the contactor coil**: hardware, independent of this firmware | L5 cutout | 27 |
| Over-temperature | `SR-09`: heat off at the limit, latch at +10 °C | `SR-10` setpoint excursion | L5 cutout | 10, 11 |
| Enclosure over-temperature | `SR-11`: > 70 °C | `SR-12` insulation-degradation warning |, | 12, warning 101 |
| Control or safety task overrun | `SR-13`: cycle not completed within 2 × period | Task watchdog (`SR-14`) | Charge-pump decay (L3) | 13, 14 |
| MCU hang, deadlock or panic | Task + RTC watchdog |, | **Charge-pump decay, no code involved** | 15 |
| Brownout or power loss | Brownout detector (`SR-15`) | Pull-downs (`HR-08`) | Charge-pump decay |, |
| Configuration corrupt or storage failure | `FR-CFG-05` | Defaults, and refusal to run |, | 20 |
| CT disconnected or failed | `FR-CUR-11` after its grace window | Thermal rules carry the load alone, warning 111 raised | L5 cutout | 26, warning 111 |
| Relay becoming intermittent before outright failure | `SR-30`: self-clearing mismatch episodes, switching-operation life limits |, |, | warnings 109, 110 |

Two entries deserve to be read twice:

- **Contactor welded has no backstop inside the boundary.** Once both series
  devices conduct, the controller has no remaining means of interrupting the
  current. This is why `SR-27` separates the welded verdict from the
  SSR-shorted one at all, why fault 22 is the more severe of the pair, and why
  its operator instruction is to **isolate the kiln at its supply**. It is
  carried as [RR-01](#8-residual-risk).
- **A hung MCU is handled by the only layer that needs no code.** This is the
  single strongest property in the design, and [§5.3](#53-l3--the-heat-authority-hardware)'s
  warning about preserving it should be treated as binding.

### 7.2 Timing

| Path | Budget | Source |
|---|---|---|
| Safety supervisor evaluation cadence | ≥ 4 Hz required; **10 Hz implemented** | `NFR-03` |
| Detectable condition → zero duty | ≤ 500 ms; **110 ms worst case** (one 100 ms safety period + one 10 ms window tick) | `NFR-04`, [architecture §6.3](architecture.md#63-fault-reaction) |
| Detectable condition → contactor open | ≤ 1 s (charge-pump decay) | `NFR-04`, `AD-05` |
| Uncommanded current → de-energised | ≤ 1 s from the offending measurement window | `NFR-27` |
| `SR-27` weld verdict | a further ≤ 3 s | `NFR-27` |
| Any non-safety activity delaying a safety cycle | ≤ 50 ms | `NFR-02`, `AD-13`, `AD-15` |
| Reset → measuring and safely off | ≤ 3 s | `NFR-09` |

The 50 ms figure is architectural, not aspirational: the control and safety
tasks sit on core 1 while WiFi, lwIP, the HTTP server and the HMI sit on core 0
(`AD-15`), and tasks communicate by queues and immutable snapshots with no mutex
on the control path (`AD-13`), which removes priority inversion and unbounded
blocking as a class.

### 7.3 Why two rules carry a confirmation window

SG-10 is a safety goal, not a usability one: **a rule that stops a healthy
firing is worse than no rule, because it gets switched off** (HZ-10). Two rules
are therefore deliberately slower than they could be, and both cases are
documented here rather than left as a surprising constant in the source.

- **`SR-08` (uncommanded heating) arms only after a 60 s settle.** After a spell
  at high duty the measured temperature keeps climbing as heat soaks inward from
  the elements; arming immediately reads that as a shorted SSR. The consequence
  must be stated plainly: during a normal firing duty is rarely zero for a full
  minute, so **`SR-08` is effectively inactive while running**. That is exactly
  why `SR-25`, which sees the same failure in amps inside a second, is the
  primary detection and `SR-08` the backstop for a kiln whose current monitoring
  is off or whose transformer has failed.
- **`SR-05` (reversed thermocouple) requires the fall to persist for 30 s.** A
  kiln with transport lag and imperfect gains overshoots and then coasts down
  several degrees while the controller is already pushing duty back up; an
  instantaneous test reads that as a reversed probe and stops a healthy firing.
  A genuinely reversed couple falls monotonically and does not come back, so the
  confirmation costs it nothing.

The same reasoning drives `fail_off_min_windows` and `deviation_min_windows` in
the current rules: a rule decided purely on a timer can be tipped over by one
unrepresentative measurement that happens to be the last before the window
expires, the first on-window of a run, caught while the contactor is still
closing. Requiring both elapsed time and a count of consecutive confirming
windows is strictly more evidence for the same conclusion, and costs a fraction
of a second.

### 7.4 Reaction and recovery

On any fault, in this order (`SR-16`):

1. **Command zero duty**: before anything that could take time.
2. **De-assert heat enable**, so the contactor opens; for `SR-27` this happens
   *ahead of any verdict*, because dropping the contactor is the measurement.
3. **Latch the fault** with its code and a snapshot, and write it to
   non-volatile storage.
4. **Sound the alarm**: after step 3, deliberately (`SR-17`).
5. **Enter Fault state**, de-energised and latched; log the event; present the
   cause on the display and in the web interface.

Recovery is deliberately obstructive, because HZ-08 is a catastrophic hazard
reached by convenience:

- A latched fault **persists across power loss** and requires an explicit
  operator acknowledgement. The system never silently self-recovers (`SR-17`).
- Acknowledgement is **refused while the triggering condition is still
  observably present** (`SR-18`), and the refusal reuses the same rule function
  as the detector, so there is no second implementation to disagree with it.
- Clearability is an **allow-list**: a fault code that is not explicitly listed
  as clearable is not clearable. A newly added fault therefore defaults to
  requiring a deliberate decision rather than inheriting "acknowledge and carry
  on" by omission.
- After a power loss mid-run, recovery follows the configured policy
  (`FR-RUN-08`) reconstructed from the log tail (`AD-09`); a recovery that
  cannot be justified is refused and raised as fault 19 rather than guessed at.
- Safety-relevant configuration cannot be changed while a run is in progress
  (`FR-CFG-08`), and no detection threshold range permits disabling a detection
  (`SR-22`).

Every fault carries a unique, stable numeric code with a human-readable cause,
presented on both the display and the web interface and documented in
[requirements appendix A](requirements.sdoc)
(`SR-19`). Codes are never reused.

## 8. Residual risk

What remains after L1–L6, stated without softening. Each entry names who carries
it.

| ID | Residual risk | Why it remains | Carried by | Mitigation in place |
|---|---|---|---|---|
| **RR-01** | **Welded contactor with a shorted SSR.** Both series devices conducting leaves the controller with no means of interrupting the heater circuit. | There is no third interrupting device inside the boundary. | **L5 cutout and the operator.** | `SR-27` discriminates the case within ~4 s and latches fault 22, whose operator instruction is to isolate at the supply; the alarm sounds; `SR-30` warns of the intermittent mismatches that precede outright relay failure. |
| **RR-02** | **A three-phase kiln cannot be fired safely with this controller.** It measures one phase, so a fault on either of the other two would be caught only by the thermal backstop, slowly, and the power and energy figures would cover a third of the load. | Three-phase support was dropped on 2026-10-06 rather than half-built, because partial electrical cover is the kind that gets trusted. | The installer, who must not fit this controller to a three-phase kiln. | Stated as out of scope in the README, in [requirements §12](requirements.sdoc) and in `ASM-10`, rather than left as a limitation to be discovered. The thermal rules still protect a single-phase kiln in full. |
| **RR-03** | **A CT fitted to the wrong conductor, or clipped around two conductors** (net current ≈ 0) makes the entire electrical channel blind or makes it trip on every run. | The controller cannot tell a correctly fitted CT reading zero from a wrongly fitted one. | The installer. | Commissioning verifies a plausible reference current before the first firing; `FR-CUR-11` distinguishes "no signal at all" from "zero current"; a disabled or failed channel raises warning 111 continuously rather than going quiet. |
| **RR-04** | **A systematic error in the safety rules themselves**: a wrong threshold, an inverted comparison, a timer that never advances, is not caught by L2 or L3, since both would be working as written. | L3 checks liveness, not correctness. | The project, through verification. | 100 % of safety decision branches covered by automated host tests (`TR-19`); every `SR` covered by at least one automated test, inspection alone insufficient (`TR-23`); rules are pure functions driven directly by tests (`TR-09`); CI blocks merge on any failure (`TR-24`). **L5 remains the only protection genuinely independent of this risk.** |
| **RR-05** | **Common-cause failure of the MCU and its rails** defeats L2, L3 and L4 together. | One MCU, one supply. | The design, by choice. | Every one of those layers fails *towards* de-energised, so the common-cause outcome is the safe state rather than an unsafe one. L5 is unaffected. |
| **RR-06** | **An operator who widens thresholds to their limits** reduces the effectiveness of detection without disabling it. | `SR-22` bounds the ranges but permits the full range. | The operator. | Ranges are bounded; `SR-23`'s 1350 °C ceiling is a compile-time constant no configuration can raise; nothing can be disabled outright; untuned gains and disabled monitoring are annunciated as standing warnings (108, 111). |
| **RR-07** | **Network-borne program edits.** `FR-WEB-26` removed every route that can heat the kiln, so a firing can no longer be started, altered or aborted over the network. What remains is that an attacker can edit a stored firing *program*, which takes effect only if an operator then starts it at the kiln. | Program authoring on a keyboard is the one thing a rotary encoder and a 128×64 OLED are genuinely bad at, so it was kept deliberately. | The owner of the network (`ASM-05`), and the operator who confirms the start. | Authentication with constant-time comparison and rate-limited failures (`NFR-19`); every request body and parameter treated as untrusted; no cloud connectivity and no remote access outside the local network by design. The device is **not** to be exposed to the internet. |
| **RR-08** | **Hot surfaces and residual heat** (HZ-11) and **mains exposure during installation or service** (HZ-07). | Inherent to the equipment. | The installation and the operator. | `HR-18` on CT isolation, creepage and clearance; `HR-16` forbidding a CT whose burden can be disconnected; documentation obligations (`NFR-26`, `SR-24`). |
| **RR-10** | **A door interlock is optional** (`HR-21` is a *should*, not a *shall*). The board now carries the series contact and the sense divider (2026-10-06), so fitting one is wiring rather than redesign, but a kiln without a switch still has nothing against HZ-13. A kiln without one is protected against HZ-13 by nothing at all, and the controller cannot tell the difference between "not fitted" and "fitted and shut" except by being told. | Many existing kilns have no interlock and retro-fitting one means working on the door furniture. | The installer. | Warning 113 is raised continuously whenever no interlock is configured, so the gap is visible on the display and in the API rather than silent. `SR-31`'s normally-closed requirement means a *fitted* switch cannot fail silently. |
| **RR-09** | **Unattended firing**, contrary to `ASM-06`, removes L6 entirely. | The firmware cannot verify attendance. | The operator. | Stated in the README, in `SR-24`, and here. Unattended firing is **not** a design goal and the layered concept above does not assume it. |

## 9. Limitations and obligations

### 9.1 What KilnControl is not

- **Not a safety-certified device.** No SIL, no performance level, no
  third-party assessment. This document is engineering reasoning, not evidence
  of conformity.
- **Not a replacement for the independent over-temperature cutout.** Explicitly
  out of scope ([requirements §12](requirements.sdoc)).
- **Not a substitute for competent mains installation**, correct supply
  protection, or correctly sized contactors and conductors.
- **Not designed for unattended firing.**

### 9.2 Obligations on the installer

| | Requirement |
|---|---|
| Fit an independent hardware over-temperature cutout in the safety chain | `HR-13`, `ASM-04` |
| Have the mains installation performed by a competent person per local regulation [R6] | `SR-24`, `ASM-04` |
| Fit a door interlock if the kiln's door furniture allows it, wired **normally closed** and in series with the contactor coil as well as to the controller input | `SR-31`, `HR-21`, RR-10 |
| Fit the CT around **one** heater conductor, downstream of both the contactor and the SSR | `FR-CUR-01`, `HR-11`, and see RR-03 |
| Use a voltage-output CT with an integral burden, or fit the burden permanently at the board, never a current-output CT whose burden can be disconnected | `HR-16` |
| Verify a plausible reference current at commissioning, before the first firing | RR-03 |
| Configure the maximum chamber temperature to the **kiln's** rating, not the controller's ceiling | `SR-23`, RR-06 |
| Keep the device on a trusted local network; do not expose it to the internet | `ASM-05`, `NFR-20`, RR-07 |

### 9.3 Obligations on anyone changing this design

| | Constraint |
|---|---|
| The heat-enable charge pump must stay a **software-generated** square wave. Never a static GPIO, never a hardware PWM peripheral. | `AD-05`, `HR-07`, [§5.3](#53-l3--the-heat-authority-hardware) |
| Heat authority stays in one place, the safety supervisor, and no other code path may grant it. | `AD-04` |
| A door interlock is hardware first and software second: `SR-31` latches and annunciates, `HR-21`'s series wiring is what actually interrupts. Do not let the software rule become the justification for dropping the wiring. | `HR-21`, `AD-05` |
| The thermal rules and the current rules are both load-bearing. Neither may be removed because the other exists. | [§6](#6-independence), [requirements §5.2](requirements.sdoc) |
| A new fault code must be given an explicit clearability decision; it does not inherit one. | [§7.4](#74-reaction-and-recovery) |
| **No part of this project may be described as MISRA-compliant.** `clang-tidy` implements no MISRA checks in any release; the `hicpp-*` module that approximated High Integrity C++ is gone from LLVM; cppcheck's free addon is MISRA **C** 2012 only and needs non-redistributable rule texts. Real MISRA C++:2023 checking is commercial. What `.clang-tidy` enforces is a high-integrity profile, which is a different and more honest claim. | `AD-20`, `.clang-tidy` |
| A change to a safety rule requires a host test that fails before it and passes after. | `TR-25` |
| Safety decision branches stay at 100 % coverage; the gate is CI-blocking, not advisory. | `TR-19`, `TR-24` |
| The core stays free of hardware, RTOS and IDF references, so every rule remains host-testable. | `TR-01`, `AD-01`, `AD-14`, enforced by `tools/layercheck` |

## 10. Verification of this concept

The safety concept is verified by the strategy of
[requirements §10](requirements.sdoc) and
[architecture §14](architecture.md#14-build-and-test-architecture). The parts
that bear specifically on the claims made above:

| Claim | How it is verified | Status |
|---|---|---|
| Every rule behaves as specified at its boundaries | Host unit tests driving each rule as a pure function, with injected time (`TR-09`, `TR-23`); `test_safety_thermal.c` and `test_safety_current.c` | Implemented |
| 100 % of safety decision branches are exercised | Coverage gate in CI (`TR-19`) | Implemented, CI-blocking |
| Rules behave correctly against a plant, not only in isolation | Closed-loop integration tests against `kiln_sim`, with thermal **and** electrical fault injection, relay fail-on, fail-off, welded contactor, partial element failure, over-current, disconnected CT (`TR-14`, `TR-27`) | Implemented |
| Latched faults survive power loss at an arbitrary instant | Host tests that cut power mid-write against a fake flash (`AD-19`), plus the persistence suite | Implemented |
| The firmware boots safe and fires correctly on the real image | QEMU job on every push, booting the real `esp32s3` image and watching it fire | Implemented |
| **The contactor releases when the safety task stops**: the central claim of L3 | **HIL: halt the safety task on a bench jig and observe the coil** | **Not yet performed** (M4 exit criterion) |
| **`SR-27` discriminates a real weld** | **HIL against an emulated welded contactor** | **Not yet performed** (M4b exit criterion) |
| Safety latency, jitter and boot time on real silicon | Instrumented timing tests on target (`NFR-02`–`NFR-04`, `NFR-09`) | Not yet performed |

The three unperformed items are all claims about **hardware**, and they are the
ones this concept leans on most heavily. Until the HIL suite has run, L3 is
verified by design review and simulation only, which is to say that the
project's strongest safety property is also, today, its least empirically
confirmed one. The M4 and M4b milestones exist to close exactly that gap, and
[`tasklist.md`](../tasklist.md) tracks the work.

## 11. Open questions affecting safety

| | Question | Effect if deferred |
|---|---|---|
| `OQ-06` | Per-phase current monitoring on a three-phase kiln, or one phase plus the thermal backstop? | RR-02 stands. To be settled before the `current` component is frozen, since it changes the ADC channel count and the rule structure. |
| `OQ-07` | Is the element positive temperature coefficient used to correct the `SR-28` baseline measured automatically on the first firing, or entered from the datasheet? | `SR-28`'s deviation bands are less discriminating across the temperature range, trading sensitivity against nuisance trips (HZ-10). |
| `OQ-05` | Is a multi-zone variant a v1.1 goal (`ASM-02`)? | A multi-zone kiln run by a single-zone controller would place zones outside the supervised path. Out of scope for v1.0 and must stay so until answered. |

---

## Appendix, Traceability

| Hazard | Safety goals | Principal requirements | Fault / warning codes |
|---|---|---|---|
| HZ-01 Uncontrolled heating | SG-01, SG-02, SG-03, SG-05, SG-09, SG-11 | `SR-01`–`SR-03`, `SR-08`, `SR-25`, `SR-27`, `SR-29`, `HR-06`–`HR-08` | 9, 21, 22, 25 |
| HZ-02 Commanded over-temperature | SG-06 | `SR-09`, `SR-10`, `SR-23` | 10, 11 |
| HZ-03 False low reading | SG-04, SG-09 | `SR-04`–`SR-06`, `FR-ACQ-12` | 1–7, 16 |
| HZ-04 Loss of control | SG-01, SG-02, SG-11 | `SR-13`, `SR-14`, `AD-05`, `NFR-02` | 13, 14, 15 |
| HZ-05 Enclosure over-temperature | SG-08 | `SR-11`, `SR-12` | 12, warning 101 |
| HZ-06 Over-current | SG-05 | `SR-29`, `HR-06` | 25 |
| HZ-07 Shock / arc exposure |, (installation) | `HR-16`, `HR-18`, `SR-24`, `ASM-04` |, |
| HZ-08 Unsupervised re-energisation | SG-07, SG-11 | `SR-16`–`SR-19`, `SR-21`, `FR-RUN-08`, `NFR-15` | 19, 20 |
| HZ-09 Undetected failure to heat | SG-05, SG-09 | `SR-07`, `SR-26`, `SR-28` | 8, 23, 24, warning 112 |
| HZ-10 Protection defeated by nuisance trips | SG-10 | `SR-22`, `SR-23`, the confirmation windows of [§7.3](#73-why-two-rules-carry-a-confirmation-window) | warnings 108, 111 |
| HZ-11 Hot surfaces, residual heat |, (operation) | `SR-24`, `NFR-26`, `ASM-06` |, |
| HZ-13 Chamber opened while firing | SG-13 | `SR-31`, `HR-21` | 27, warning 113 |
| HZ-12 Hazardous remote command | SG-12 | `NFR-19`, `NFR-20`, `ASM-05`; analysed in full in [`security.md`](security.md) |, |

| Safety goal | Protection layers | Residual risk |
|---|---|---|
| SG-01 Single-fault tolerance | L2, L3, L5 | RR-04, RR-05 |
| SG-02 Active assertion only | L3 | RR-05 |
| SG-03 Two series interrupters | L3 | **RR-01** |
| SG-04 Wrong measurements detected | L2 thermal | RR-04 |
| SG-05 Electrical detection | L2 current | RR-02, RR-03 |
| SG-06 Bounded temperature | L1, L2 | RR-06 |
| SG-07 Latch and deliberate clearing | L2, L4 | RR-09 |
| SG-08 Controller environment | L2 |, |
| SG-09 Channel redundancy | L2 thermal + L2 current | RR-02, RR-03 |
| SG-10 No nuisance tripping | L2 rule design | RR-06 |
| SG-11 Safe through transitions | L3, L4 |, |
| SG-12 Authenticated remote control | L1 (web) | RR-07 |
| SG-13 Chamber opened removes power | L2 (`SR-31`) + L3 (`HR-21` series wiring) | RR-10 |
