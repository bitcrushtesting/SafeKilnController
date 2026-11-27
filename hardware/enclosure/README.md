<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# Enclosure

Panel-mount housing for the Safe Kiln Controller, in three printed parts.

```sh
./slice.sh              # render and slice all three for PLA
./slice.sh knob         # just one
./slice.sh --stl-only   # STLs only, slice them yourself
```

| Part | Size | 0.2 mm PLA | Prints |
|---|---|---|---|
| `shell` | 54 × 81 × 115 mm | 8 h 49 m | open face up, no support |
| `bezel` | 70 × 97 × 7 mm | 1 h 40 m | face down, no support |
| `knob` | 25 × 25 × 16 mm | 27 m | bore up, no support |

Panel cut-out **54.4 × 81.3 mm**; outside size 70 × 97 × 115 mm.

## ⚠ PLA is probably the wrong material here

PLA softens at **55 to 60 °C**. This box bolts to the panel of a kiln, and
`SWR-SAF-11` exists because that panel can get hot enough to matter: the
firmware raises a fault on enclosure over-temperature at a configurable limit
whose default is well inside PLA's softening range.

So print PLA for **fit, feel and review**, which is what the knob's bore and
the panel cut-out need, and it is cheap and accurate. Before a real
installation, measure the panel at temperature, and if it runs warm use PETG
or ASA. Both slice from the same profile with their own filament settings;
nothing in the geometry assumes PLA.

A softened enclosure is not a safety failure on its own: nothing in the
protection chain depends on the plastic. It is a mounting failure, which means
a board hanging off a hot panel by its wiring.

## Print it in this order

1. **The knob**, alone, and try it on the encoder. `knob_bore` is 6.15 mm,
   which suits a splined EC11E shaft on a well-calibrated machine; if it is
   tight or loose, change that one number. Four minutes, and it saves
   discovering the same error at the end of an 8 hour print.
2. **The bezel**, which is the part you look at. Check the display window
   against the module you actually bought before printing the shell: the three
   `CHECK` parameters in the `.scad` are the common 0.96″ variant and modules
   differ.
3. **The shell**, last, because it is the long one.

## What the design is built around

Every dimension comes out of `hardware/safekiln.kicad_pcb`, not from a guess,
and the board's own placement decides the shape:

- the outline is **100 × 70 mm**;
- **SW3, the rotary encoder, is on the board** at its left edge, so that edge
  is the operator's face and the board stands on edge behind the panel;
- `J1` (5 V in) and `J2` (USB-C) are on that same edge, under the knob;
- six 5 mm terminal blocks leave the **right** edge (coil, SSR, lid, CT and
  the two thermocouples), so that edge faces into the back of the box, which
  is where the kiln's wiring belongs;
- `J5` is an OLED module **on a cable**, so the display is the one thing free
  to go where it should.

The display sits in its own lane beside the board's column rather than above
the knob, because the front face is only as wide as the board's thickness plus
that lane, and the first version of this model put a 27 mm module on a 23 mm
face. The render caught it; see the note at `in_w` in the `.scad`.

## Before this can be screwed together

**The PCB has no mounting holes.** Nothing fixes the board to anything, so the
four M3 standoffs in the shell are a *proposal* the board has to adopt: holes
4 mm in from each corner of the 100 × 70 outline. Tracked as `HWR-24`.

The supervisor's SWD tag-connect (`J4`) needs a probe to reach it through the
shell's side opening. That opening is positioned from `J4`'s coordinates, but
a TC2030 needs the probe held square to the pads, so check the clearance with
the probe in hand before trusting it (`HWR-19`).

## The wall-mount housing

`safekiln-mainsbox.scad` is a wall-mount housing in two parts, stacked front
to back: a **base** on the wall that carries everything at 230 V, and a
**top** in front of it that carries the controller board flat behind its
face. It is for a **single-phase kiln of up to 16 A at 230 V (3.6 kW)**.

```sh
./slice.sh mainsbase mainstop knob
```

| Part | Size | Prints |
|---|---|---|
| `mainsbase` | 153 x 218 x 77 mm, 185 mm across the ears | back down, brim, no support |
| `mainstop` | 153 x 218 x 30 mm, plus a 4 mm skirt | face down, no support |
| `knob` | 26 x 26 x 16 mm, the panel enclosure's | bore up, no support |

Assembled it is 153 x 218 x 106 mm. Print both parts in ASA (or a
flame-retardant grade, see below); `pla-0.2mm.ini` is for fit checks only.

### ⚠ Printed plastic is not a mains enclosure by default

A part that holds live terminals has to pass the glow-wire test of
EN 60695-2-11 or be UL 94 V-0. PLA, PETG and ordinary ASA are neither. For a
real installation, print the base in a flame-retardant grade (ASA-FR, PC-FR,
PC/ABS V-0) or use the model to check the layout and build it into a
commercial box. Installing it is work for a competent person (`UR-007`).

### ⚠ There is no barrier between the two parts

The top is open at the back, so the board's back faces the mains devices
across open air. The separation between SELV and 230 V rests on clearance
(the board and its leads stay in the top, at least 2 mm in front of the
base's rim, and the model asserts it) and on wiring discipline: every mains
conductor ferruled, every mains wire tied so that one which comes loose
cannot swing into the top. That is a weaker argument than a wall. If the
housing is to be more than a prototype, add a printed barrier plate over the
base, with a grommet for the 12 V wires.

### What goes in it

| | Part | Rating | Margin at 16 A |
|---|---|---|---|
| -G1 | RECOM RACM15E-12SK/PMAD | 12 V 1.25 A, 100 to 240 V AC in, 83 x 26.4 x 29.5 mm, 4 x M3 | about 0.3 A drawn, see below |
| -K1 | Hongfa HF92F-012D-2C, QC terminals, flange | 2 Form C, NO 30 A 250 V AC resistive, 12 V coil 1.7 W | 1.88 |
| -V1 | COMUS WG A5 6D 40 Z | zero-cross, 40 A, 24 to 280 V AC, 3 to 32 V DC input | 2.5 |
| -E1 | heatsink, at most 1.5 K/W, fins vertical, at most 66 mm deep along the wall | | see below |
| | SSR terminal cover, thermal pad | | |
| -F1 | fuse terminal, 5 x 20 mm, with T 1 A | protects the 0.75 mm² supply branch | |
| -X1 to -X6 | Phoenix Contact UT 4 (x4), UT 4-PE (x2), 2 end brackets | 4 mm², 32 A | |
| | TS 35 x 7.5 rail, 76 mm | | |
| | 2 x M25 cable gland (9 to 17 mm) in the base, 3 x M16 (4 to 8 mm) in the top, with locknuts | | |
| | 10 x M3 heat-set insert, 2 x M4 heat-set insert | | |
| | 4 x M3 x 25 (top to base), 4 x M3 x 8 (-G1), 2 x M4 x 10 (-K1), 4 x M3 self-tapping (board), 4 x M2 x 6 (display), 4 x M4 for -E1 | | |

The outlines of -G1, -K1 and -V1 in the model come from their datasheets.
The HF92F's ordering code carries a termination digit after `2C` (`2C21F`,
`2C31F` and so on). The model assumes type 2, QC tabs with a slotted flange,
so check the code on the relay you have.

**Why -K1 breaks both poles.** A kiln on a Schuko plug can have L and N
swapped by turning the plug over. A 1-pole break would then open the neutral
and leave the elements live, so -K1's two normally-open contacts break both.

**Why -V1 is on an outside heatsink.** The WG A5 drops about 1.85 V peak,
roughly 1.2 W per amp, so about 20 W at 16 A. In a closed plastic box that
raises the air by tens of kelvin, and `SWR-SAF-11` trips on enclosure
temperature. So its base plate goes through the base's right wall onto -E1
outside. With -E1 at 1.5 K/W and about 0.2 K/W through the pad, a 45 °C room
puts the SSR's base at about 79 °C. Check that against COMUS's derating at
16 A.

**The 12 V budget.** The ESP32-S3's radio bursts through the buck come to
about 0.11 A at 12 V. -K1's coil draws about 0.14 A (1.7 W), the SSR input at
most 10 mA and the buzzer about 30 mA. That is about 0.3 A against -G1's
1.25 A, and the coil sits well inside F2's 0.5 A hold current (`HWR-14`).

### Layout

Seen from the front, X to the right and Y up the wall:

- **Base.** A 76 mm DIN rail low down carries -F1 and the six terminals, with
  the two M25 glands under it in the floor. Above the rail, -G1 stands
  upright on four bosses, AC end and latch down, and -K1 stands beside it on
  two. -V1 sits in the right wall beside -K1, with -E1 outside.
- **Top.** The board lies flat 13.8 mm behind the face, component side
  forward, on four standoffs at the `HWR-24` holes. The encoder shaft comes
  straight through: the knob drops into a hole in the face, down to just
  above the bushing, and takes 8 mm of the shaft. The display is screwed to
  four bosses behind its window, so the header only carries the signals. A
  duct in the left wall reaches the USB-C socket. The thermocouple and
  lid-switch cables enter through three M16 glands in the top's floor.

All cables enter from below, so water running down a cable drips off the
gland. Vents are 2 mm slots (IP3X), none in anything that faces up.

The display is turned 90 degrees. J5 is a vertical column of four sockets,
so a standard 0.96″ module plugged into it lies on its side, header to the
left. The window follows the module, and the firmware has to draw rotated
to match.

### Wiring

Mains, 2.5 mm² H07V-K with ferrules unless noted:

| From | To | |
|---|---|---|
| supply L, N, PE | -X1, -X2, -X3 | M25 gland |
| -X1 | -F1, then -G1 CON1.1 (L) | 0.75 mm² after -F1 |
| -X2 | -G1 CON1.2 (N) | 0.75 mm² |
| -X1 | -K1 pole 1, common | |
| -K1 pole 1, NO | -V1 1 | |
| -V1 2 | -X6, through the CT | the CT is on L, downstream of -K1 and -V1 (`SYS-HW-11`) |
| -X2 | -K1 pole 2, common | |
| -K1 pole 2, NO | -X5 | |
| -X4 | kiln PE, and -E1 | ring lug on an M4 into -E1; -X3 and -X4 bond through the rail |
| -X5, -X6, -X4 | kiln | second M25 gland |

Take the pole terminals from Hongfa's wiring diagram for 2 Form C, on the
relay in hand. The normally-closed contacts stay unused.

12 V, 0.5 mm² H05V-K. The insulation is rated for 300/500 V because these
wires run past the mains devices. Tie them away from the mains terminals.

| From | To | Board net |
|---|---|---|
| -G1 CON2.3 (+), CON2.4 (-) | J1.1, J1.2 | `V_IN`, `GND` |
| J6.1, J6.2 | -V1 3 (+), -V1 4 (-) | `+12V`, `SSR_DRV` |
| J8.1, J8.2 | -K1 coil | `LID_SWITCH`, `COIL_DRV` |
| J10 | CT secondary | `CT_IN`, `VBIAS` |
| J9 | lid switch, normally closed | M16 gland in the top |
| J3, J7 | thermocouples | M16 glands in the top |

### Assembly

1. Press the heat-set inserts: four in the base's rim, two in the rail
   bosses, four in -G1's bosses, two M4 in -K1's. Screw the rail, -G1 and
   -K1 down.
2. Drill -E1 for -V1, and drill and tap it M4 at the four screw positions
   the render prints (`HEATSINK SCREWS`, from the SSR's centre). Mount -V1 on
   -E1 with the thermal pad.
3. **Wire -V1 before it goes in.** Its terminals face into the base, so
   connect -V1 1, 2, 3 and 4 with leads long enough to reach -K1, the CT and
   J6. Then pass -V1 through the wall and screw -E1 on from inside.
4. Fit the glands and wire the base as above. Tie every mains wire down.
5. In the top: screw the display module to its four bosses first, glass to
   the window, then lower the board onto its standoffs so J5 takes the
   module's header, and screw it down.
6. Wire the 12 V side to the board, close the top onto the base with the four
   M3 x 25, and push the knob onto the shaft through the face.

### Still to check, marked CHECK in the model

- `hs`: the heatsink's real outline.
- `enc_shaft_h`, `enc_bush_h`: the encoder's shaft and bushing heights. They
  set how much shaft the knob takes; the model asserts at least 6 mm.
- `oled_*`: the module's outline, holes, window and the height of the
  header stack, which sets how far the board sits behind the face.
- `relay_recept`: the depth of a 6.3 mm receptacle on -K1's tabs.

## Files

| | |
|---|---|
| `safekiln-enclosure.scad` | all three parts, parameterised, `-D part="…"` |
| `safekiln-mainsbox.scad` | the wall-mount housing, base and top, `-D part="base\|top\|knob\|open\|all"` |
| `pla-0.2mm.ini` | PrusaSlicer configuration, flat so `--load` applies it |
| `slice.sh` | render, slice, and verify the profile was actually used |

`slice.sh` checks more than it looks like it should, and each check is there
because the failure it catches is silent: a render that is not a closed solid,
a G-code file too small to be a part, a slicer that exits **zero** while
refusing a configuration and writing nothing, and a profile that was ignored.
The last one happened: the first `pla-0.2mm.ini` used `[print:]`-style
sections, which makes it a config *bundle*, and `--load` ignored it without a
word and sliced at 0.3 mm with a cold bed. The script now reads the settings
back out of the finished G-code.
