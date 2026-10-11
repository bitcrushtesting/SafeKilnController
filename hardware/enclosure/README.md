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

## Files

| | |
|---|---|
| `safekiln-enclosure.scad` | all three parts, parameterised, `-D part="…"` |
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
