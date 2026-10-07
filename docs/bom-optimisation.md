<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# BOM optimisation candidates

Parts the design might do without, or do more cheaply, each worked through to a
recommendation rather than left as a hunch.

This is not a shopping list, and the test applied here is not "what is the
cheapest part that measures this". In a safety-relevant design an expensive part
is usually paying for something other than its obvious function, and the useful
question is whether it is still paying for that. Sometimes it is. Sometimes the
thing it was bought for has been weakened by a later decision and nobody went
back to the part.

Nothing in this document is a decision. Each candidate ends with a
recommendation and the list of requirements a change would touch, so the
decision can be taken deliberately and its blast radius is known before anyone
opens the schematic.

## How a candidate is assessed

1. **Which requirement puts the part there?** If no requirement names it, that
   is itself the finding.
2. **Is it over-specified for the job as stated?** Measured against the
   configured range the firmware actually uses, not the datasheet.
3. **What else is the part buying?** Fault outputs, isolation, placement
   freedom, a shared driver, a hardware safety path.
4. **What breaks if it goes, and what would have to change on paper?**

---

## 1. TC2, the enclosure thermocouple channel

**Status: open, and narrowed.** Tasklist `R3` has since declined the option of
repurposing this channel as a second chamber couple, so what remains is the
original question: delete it, or replace it with a cheap I²C part. Option A
below also got easier, because the supervisor's link already carries the
chamber front end's cold-junction temperature, which is the reading option A
proposes to use for `SR-11`. Decide alongside `K2`.

### 1.1 What the channel is for

Two requirements put it there. [`FR-ACQ-09`](requirements.sdoc) asks for
enclosure/electronics temperature on "a second, independently configurable
thermocouple channel", and [`SR-11`](requirements.sdoc) is the rule that
consumes it: latch a fault above the configured maximum, "protecting the
controller and its wiring".

It is genuinely safety-acting, not instrumentation. The limit is configurable
over 40 to 90 °C with a default of 70; the channel carries the same
open-circuit and front-end fault detection as the chamber channel through
`rule_tc`; and `sense.case_present` can stand the whole rule down for a build
without the probe fitted.

### 1.2 The front end is over-specified for the band it measures

A MAX31856, a type-K thermocouple, and the protected analogue front end
[`HR-15`](requirements.sdoc) requires, to read a band between 40 and 90 °C.

It is worse than simply generous. A type-K couple near ambient sits in the
weakest part of its range: at roughly 41 µV/°C, a junction at 70 °C against a
cold junction at 25 °C produces about 1.8 mV, and the *absolute* reading is
therefore dominated almost entirely by cold-junction compensation accuracy
rather than by the measurement. The design is using a thermocouple to measure
something barely above its own cold junction.

A TMP117 (±0.1 °C over this band) or an STTS22H for a few cents would be more
accurate here, not less. The I²C bus already exists on the board (`SDA` on
IO8, `SCL` on IO18) and carries only the SSD1306 display at 0x3C, so there is
bus capacity, an existing driver, and no new interface to bring up.

### 1.3 What the cost is actually buying

Not the measurement: the `~FAULT` pin.

[`HR-24`](requirements.sdoc) puts both thermocouple front ends' fault outputs
in series into the contactor coil, each through its own switching element, so
that a reported sensor fault removes the heater without the firmware's
involvement. That is the interlock chain of tasklist section K, whose parts
(`Q5`, `Q6`, `R28` to `R31`) have just reached the PCB. A plain I²C temperature
sensor has no comparable output, so swapping the part removes the enclosure
branch of that chain.

That is the real reason the part is there, and any assessment that only
compares measurement performance is answering the wrong question.

### 1.4 Why that justification is already weak

The project's own notes undercut it twice.

`K1` records that the thermocouple interlock is **not** firmware-independent:
the `FAULT` outputs are open-drain, so an unpowered or absent front end leaves
the path closed, and the MAX31856 detects an open circuit only once its fault
mask has been configured, which means the interlock does not act out of reset.
Firmware `SR-04` remains the cover for a dead or unconfigured front end.

`K2` goes further and asks whether an enclosure thermocouple fault should stop
a firing at all, proposing a fitted `0R` in `Q5`'s drain so the branch can be
depopulated without cutting a track. The reasoning there is that `SR-11` is a
backstop, and a failed enclosure probe killing a firing mid-glaze is a nuisance
trip, which `HZ-10` identifies as how protections come to be disabled.

So the capability TC2's front end uniquely provides is both qualified and
already under review. If `K2` resolves towards depopulating the branch, the
MAX31856 on that channel has no justification left at all.

### 1.5 The measurement may already exist, unused

The chamber front end measures its own cold junction, and that die is on the
PCB. `cj_c` is read in `hal_tc.cpp`, passed up through `app.cpp`, and is
already a field of `kiln_app_view_t`.

It is, in other words, an electronics temperature that the firmware already
has and does not use for anything safety-related. Given that `SR-11`'s stated
intent is protecting "the controller and its wiring", this is plausibly the
measurement the requirement is asking for, available today at no cost in parts.

The caveat is honest: the MAX31856's cold junction tracks the temperature of
that package, which is a good proxy for the board and a poor one for a hot spot
somewhere else in the enclosure. Whether that is adequate depends on what
`SR-11` is really protecting, which is worth settling explicitly either way,
because it is the question underneath this whole entry.

### 1.6 Options

| | Option | Effect |
|---|---|---|
| **A** | **Drop TC2. Use `cj_c` for `SR-11`, and put a bimetallic thermal cutout (normally closed, around 85 °C) in series with the coil if a hardware trip is wanted.** | Removes a MAX31856, a thermocouple, a connector, one protected front end, and the enclosure branch of section K. The cutout costs cents, needs no bus, driver or configuration, and is *genuinely* firmware-independent, which is more than the path `K1` criticises can claim. |
| **B** | **Keep a dedicated sensor, make it I²C (TMP117 or STTS22H), on-PCB near the SSR drive and terminals.** | Cheaper and more accurate than the present channel, keeps a sensor that can be placed deliberately, and loses the enclosure branch of the hardware chain, which `K2` wants depopulatable in any case. The adapter can still populate `case_fault_bits`: a NAK or a timeout is arguably better fault detection than an open thermocouple, because a missing I²C device is unambiguous. |
| **C** | **Status quo.** | Justified only if the enclosure branch of the hardware interlock is decided to be load-bearing, in which case `K1`'s caveats should be addressed too, since the branch does not currently do what it appears to. |

Preference is **A**, then **B**. Option A is the one that removes parts rather
than substituting them, and it replaces a qualified hardware interlock with an
unqualified one.

### 1.7 What a change would touch

This is a specification change, not a BOM substitution, because
[`FR-ACQ-09`](requirements.sdoc) names a thermocouple explicitly.

On paper: `FR-ACQ-09` (the channel's type), [`HR-02`](requirements.sdoc) (both
front ends are MAX31856 on a shared SPI bus), [`HR-24`](requirements.sdoc) (the
fault outputs in the coil chain), and `SR-11`'s verification method.

In firmware: the second `port_tc` channel, and the `sense.case_tc_type`,
`sense.case_cal_offset_c`, `sense.case_cal_gain` and `sense.case_present`
configuration items, all of which are `SAFE` and so refused mid-run by
`FR-CFG-08`.

In hardware: `Q5` and `R28` to `R31` of the interlock chain, the TC2 front end
and its `HR-15` protection network, and connector `J4` ("TC2 CASE (K)").

### 1.8 Where it is wrong to go cheap

If the hot spot that matters is remote, on the SSR heatsink or the contactor
rather than on the controller board, then an on-PCB I²C part measures the wrong
thing and option B is a false economy.

In that case the answer is still not I²C. A single-ended bus down a cable
beside a switching multi-kilowatt load is a poor choice, and
[`HR-03`](requirements.sdoc) already records a concern about a peripheral
stalling a shared bus. Prefer a DS18B20 on a twisted pair, or an NTC into a
spare ADC channel, both of which tolerate cable length and noise far better.

---

## 2. Candidates recorded but not assessed

These are already open hardware items with a cost dimension. They are listed
here so the BOM question is visible, but none has been worked through in the way
section 1 has.

| Item | The BOM question |
|---|---|
| `A9` | The 5 V contactor coil. Mains contactors are commonly 24 V or 230 V coil parts; pinning `J8` to 5 V narrows the field sharply and may cost more than it saves. |
| `A16` | The AMS1117's dropout and thermal margin. If it is marginal, the replacement is a different regulator, which is a BOM change arrived at from a different direction. |
| `A18` | The "(opt)" annotations. A part labelled optional that a requirement makes mandatory is a BOM that disagrees with the specification. |
| `K3` | `D7`, the unwired coil indicator LED. Either wire it with its series resistor or remove it; an unwired part is cost without function. |
