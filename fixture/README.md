<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Bed-of-nails fixture

The hardware-in-the-loop fixture for the controller board, built from the board
itself by [pinside](https://github.com/bitcrushtesting/pinside). This is the
fixture `docs/test-concept.md` wants and that the M4 and M4b milestones need:
`safety.md` §10 still lists the central claim of the design, that the contactor
releases when the safety task stops, as **not yet performed**, and nothing in
this repository can perform it without a jig.

## What is here

| | |
|---|---|
| `kilncontrol-fixture.json` | the fixture definition. **This is the source**; everything else is generated from it |
| `pinside-baseline.json` | findings on the DUT that have been looked at and accepted, with the reason for each |
| `board/` | a KiCad project for the fixture board, generated |
| `firmware/` | firmware for the fixture's controller, generated |

`board/` and `firmware/` are generated verbatim and are regenerated, not edited.
pinside derives footprint UUIDs from the config rather than randomising them, so
an unchanged config produces a byte-identical project and the diff of a real
change is only the change.

## Regenerating

```sh
pip install pinside                       # or run from a checkout, see its README
pinside check    hardware/safekiln.kicad_pcb --baseline fixture/pinside-baseline.json
pinside project  fixture/kilncontrol-fixture.json --out fixture/board
pinside generate fixture/kilncontrol-fixture.json --out fixture/firmware
bash fixture/firmware/test/run.sh         # 128 host checks, no hardware needed
```

`project` needs KiCad installed, because a schematic embeds a copy of every
symbol it places. `check` and `generate` do not. The board file is only ever
read; pinside never writes KiCad sources.

## Two decisions worth knowing

**The probe is a Mill-Max 0906, not the default 0985.** With the 0985's 2.54 mm
minimum pitch, eight probe pairs on this board collide: TP2-TP3, TP6-TP9,
TP7-TP8, TP15-TP16, TP19-TP20, TP20-TP22, TP21-TP34 and TP32-TP36 all sit at
exactly 2.50 mm, because the test points are on a 2.5 mm grid and the receptacle
body is wider than that. The 0906's 1.91 mm pitch clears all eight, and its
lower 0.55 N force halves the plate load as a side effect. Its dimensions are
pinside's reading of the Mill-Max catalogue and **nobody has checked them
against a supplier drawing**; do that before ordering.

**The fixture carries the RP2350 itself rather than a Pico 2 module.** 28 test
points need more channels than a Pico 2's 26 header GPIO, and the first attempt
failed on exactly that: `COIL_PERMIT` and `SUPERVISOR_RESET` had nowhere to go,
with one GPIO spare. Those two are the permit line and the supervisor's reset,
which are the whole point of a safety fixture, so the carrier is `bare` with an
`rp2350b` (48 GPIO).

## What still blocks building it

**The board has no mounting holes, and that is two problems rather than one.**
`PS050` reports it as "the DUT can pivot or rock and contact becomes
intermittent", which is the mechanical half. The other half is quieter: on a
pinside fixture **the ground return goes through the grounded mounting holes**,
which is why `GND` is not a channel and why only 27 of the 28 test points get a
probe. pinside's own example board has its mounting holes on the `GND` net. With
no holes at all, the fixture's ground pour connects to the DUT through nothing,
and `PS031`, "only one ground test point", is the same problem seen from the
other side.

So before a fixture can be ordered the controller board needs **at least three
plated mounting holes on the GND net**, positioned so the probes fall inside
their span (`PS052` watches for that). Both findings are deliberately left out
of the baseline so they keep failing until that happens.

**Two analogue channels are placed but cannot be used.** `TC1_P` and `TC1_N` are
the thermocouple's millivolt inputs. The probes are placed so the pads are
reachable, and both are configured as observed inputs that the fixture **never
drives**: a GPIO asserting 3V3 into a thermocouple input can damage the front
end. Injecting a temperature, which is what a HIL suite actually wants, needs a
calibrated millivolt source the fixture does not have. `CURR_SENSE` is in the
same position and should become an ADC channel once the conditioning scale is
taken from the schematic; it is left as an observed input rather than having a
scale invented for it.

## The directions were reviewed, and the schematic is the authority

`pinside init` drafts a config and says to treat it as a draft. It defaulted
almost every signal to `open_drain` with `active_low`, which is a safe default
because `released` means not driving, but it is a default rather than an
analysis. The directions here were set by hand from the net names:

- **13 observed inputs**, every signal the DUT drives: `SSR1`, `HEAT_EN`,
  `PUMP_NODE`, `HEAT_EN_DC`, `COIL_DRV`, `COIL_PERMIT`, `ALARM`, `TC1_DRDY`,
  `TC1_FAULT`, `TC1_CS`, plus the three analogue ones above. A fixture that
  drove any of these would contend with the board.
- **6 driven**, the signals the DUT reads: `ENC_A`, `ENC_B`, `ENC_BTN`,
  `ESP32_EN`, `ESP32_IO0`, `SUPERVISOR_RESET`. Open-drain and active-low suits
  all six: each sits on a pull-up, and released means the fixture is holding the
  board in no particular state.

`TC1_CS` deserves its own note. The config declares an SPI peripheral in master
role, so the fixture *can* drive that bus, but chip select is left as an input
because the controller drives it in normal operation. Mastering the bus means
holding the DUT in reset through `ESP32_EN` first, and that interlock is not
configured here: it is a deliberate act, not a default.

These are a reading of the net names and of the firmware, not of the schematic.
**Check them against the schematic before power is applied to a real board**,
because the failure mode of a wrong direction is two drivers fighting.
