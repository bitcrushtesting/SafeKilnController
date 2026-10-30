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

## The mains box

`safekiln-mainsbox.scad` is a wall-mount box that holds everything: the
controller, its 12 V supply, the safety contactor and the SSR. It is for a
**single-phase kiln of up to 16 A at 230 V (3.6 kW)**.

```sh
./slice.sh mainsbox mainslid stemknob
```

| Part | Size | 0.2 mm, 3 perimeters | Prints |
|---|---|---|---|
| `mainsbox` | 200 x 195 x 138 mm, 232 mm across the ears | 46 h | back down, brim, no support |
| `mainslid` | 200 x 195 x 17 mm | 7 h 30 m | face down, no support |
| `stemknob` | 26 x 26 x 28 mm | 39 m | cap down, no support |

### ⚠ Printed plastic is not a mains enclosure by default

A part that holds live terminals has to pass the glow-wire test of
EN 60695-2-11 or be UL 94 V-0. PLA, PETG and ordinary ASA are neither. For a
real installation, print the box and lid in a flame-retardant grade (PC-FR,
ASA-FR, PC/ABS V-0) or use the model to check the layout and build it into a
commercial box. Installing it is work for a competent person (`UR-007`).

### What goes in it

| | Part | Rating | Margin at 16 A |
|---|---|---|---|
| -G1 | Mean Well HDR-15-12 | 12 V 1.25 A, 85 to 264 V in, class II, DIN, 17.5 mm | about 0.35 A drawn, see below |
| -K1 | Finder 22.32.0.012.4340 | 2 NO, 25 A AC-1, 12 V AC/DC coil, DIN, 17.5 mm | 1.56 |
| -V1 | Crydom D2440 | zero-cross, 40 A, 24 to 280 V AC, 3 to 32 V DC input | 2.5 |
| -E1 | heatsink, at most 1.5 K/W, fins vertical | for example Crydom HS151 | see below |
| | SSR terminal cover, thermal pad | Crydom KS100, or equivalent | |
| -F1 | fuse terminal, 5 x 20 mm, with T 1 A | protects the 0.75 mm² supply branch | |
| -X1 to -X6 | Phoenix Contact UT 4 (x4), UT 4-PE (x2), 2 end brackets | 4 mm², 32 A | |
| | TS 35 x 7.5 rail, 116 mm | | |
| | 2 x M25 cable gland (9 to 17 mm), 3 x M16 (4 to 8 mm), with locknuts | | |
| | grommet for a 14 mm hole in 3 mm | | |
| | 8 x M3 heat-set insert, 6 x M3 x 10 countersunk, 4 x M4 for -E1 | | |

Check the order codes against the supplier before ordering. They are the
parts this was designed for, and the DIN outlines in the model are catalogue
figures, not measurements.

**Why a 2-pole contactor.** A kiln on a Schuko plug can have L and N swapped
by turning the plug over. A 1-pole break would then open the neutral and
leave the elements live, so -K1 breaks both.

**Why -V1 is on an outside heatsink.** A D2440 dissipates about 1.2 W per
amp, so about 20 W at 16 A. In a closed plastic box of this size that raises
the air by tens of kelvin, and `SWR-SAF-11` trips on enclosure temperature.
So the SSR's base plate goes through the right wall onto -E1 outside, and what
stays inside is a few watts from the coil, the supply and the contacts. With
-E1 at 1.5 K/W and about 0.2 K/W through the pad, a 45 °C room puts the SSR's
base at about 79 °C. Check that against the D2440's derating curve at 16 A.

**The 12 V budget.** The ESP32-S3's radio bursts through the buck come to
about 0.11 A at 12 V. -K1's coil draws about 0.17 A (around 2 W, check
Finder's datasheet for the 012 coil), the SSR input about 15 mA and the
buzzer about 30 mA. That is about 0.35 A against -G1's 1.25 A, and the coil
sits well inside F2's 0.5 A hold current (`HWR-14`).

### Layout

Two bays with a barrier between them that runs from the back plate to the
lid. Two ribs on the lid straddle its top edge, so there is no straight gap
between the voltages.

- **SELV bay, left.** The controller stands on edge, screwed to the left
  wall at the `HWR-24` holes, so the barrier has no screw holes. The encoder,
  USB-C and display come through the lid. The board sits 15 mm behind the
  lid, which leaves room for J1's ferrule. The knob reaches the shaft on a
  12 mm stem, and the lid has a duct down to the USB-C socket so a plug
  reaches it.
- **Mains bay, right.** -G1 and -K1 sit next to the barrier, where their
  12 V wiring crosses. -F1 and the terminals are to their right, above the
  two M25 glands. -V1 sits in the right wall, high up and towards the lid.
  That is clear of every device front and every terminal screw, and the
  model asserts both.

All cables enter through the floor, so water running down a cable drips off
the gland. Vents are 2 mm slots (IP3X) in the floor and high in both sides,
none in the top.

### Wiring

Mains, 2.5 mm² H07V-K with ferrules unless noted:

| From | To | |
|---|---|---|
| supply L, N, PE | -X1, -X2, -X3 | M25 gland |
| -X1 | -F1, then -G1 L | 0.75 mm² after -F1 |
| -X2 | -G1 N | 0.75 mm² |
| -X1 | -K1 1 | |
| -K1 2 | -V1 1 | |
| -V1 2 | -X6, through the CT | the CT is on L, downstream of -K1 and -V1 (`SYS-HW-11`) |
| -X2 | -K1 3 | |
| -K1 4 | -X5 | |
| -X4 | kiln PE, and -E1 | ring lug on an M4 into -E1; -X3 and -X4 bond through the rail |
| -X5, -X6, -X4 | kiln | second M25 gland |

12 V, 0.5 mm² H05V-K. The insulation is rated for 300/500 V because these
wires run in the mains bay. Tie them away from the mains terminals.

| From | To | Board net |
|---|---|---|
| -G1 +V, -V | J1.1, J1.2 | `V_IN`, `GND` |
| J6.1, J6.2 | -V1 3 (+), -V1 4 (-) | `+12V`, `SSR_DRV` |
| J8.1, J8.2 | -K1 A1, A2 | `LID_SWITCH`, `COIL_DRV` |
| J10 | CT secondary | `CT_IN`, `VBIAS` |
| J9 | lid switch, normally closed | M16 gland |
| J3, J7 | thermocouples | M16 glands |

### Assembly

1. Press the heat-set inserts into the six lid bosses and the two rail
   bosses. Screw the rail down.
2. Drill -E1 for -V1, and drill and tap it M4 at the four screw positions
   the render prints (`HEATSINK SCREWS`, from the SSR's centre). Mount -V1 on
   -E1 with the thermal pad.
3. **Wire -V1 before it goes in.** Its terminals face into the bay, so
   connect -V1 1, 2, 3 and 4 with the leads long enough to reach -K1, the CT
   and J6. Then pass -V1 through the wall and screw -E1 on from inside.
4. Fit the board, the DIN devices and the glands, and wire them as above.
5. Fit the lid, then press the stem knob onto the shaft through it.

### Still to check, marked CHECK in the model

- `ssr` and `hs`: the SSR's and heatsink's real outlines.
- `enc_shaft_tip`: how far the encoder shaft stands beyond the board's edge.
  It sets the stem length.
- `enc_axis_off`, `usb_axis_off`: where the shaft and the USB socket sit
  across the board's thickness.
- The OLED module dimensions, as in the panel enclosure.

## Files

| | |
|---|---|
| `safekiln-enclosure.scad` | all three parts, parameterised, `-D part="…"` |
| `safekiln-mainsbox.scad` | the mains box, its lid and the stem knob, `-D part="box\|lid\|knob\|open\|all"` |
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
