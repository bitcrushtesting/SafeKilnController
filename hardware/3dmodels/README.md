<!--
SPDX-FileCopyrightText: 2026 Bitcrush Testing
SPDX-License-Identifier: GPL-3.0-or-later
-->

# 3D models

Three parts whose models KiCad references but has never shipped. Generated,
not hand-made:

```sh
python3 tools/fetch3d.py --self-test   # the maths, no network
python3 tools/fetch3d.py               # refetch and rewrite the .wrl files
python3 tools/fetch3d.py --check       # are the committed files present
```

| File | LCSC | Fitted as |
|---|---|---|
| `terminalblock_kf301_5.0_2p.wrl` | `C474881` | J1, J3, J6, J7, J8, J9, J10 |
| `rotaryencoder_pec11r_4215k.wrl` | `C7275025` | SW3 |
| `usbc_hro_type-c-31-m-12.wrl` | `C165948` | J2 |

## Why these three are not in the KiCad libraries

Not a local installation problem, which is the first thing to rule out.
Upstream `kicad-packages3D` carries `TerminalBlock_Altech`,
`_Ningbo-Kagnex`, `_Phoenix` and `_Wuerth` but no plain
`TerminalBlock.3dshapes`, no `Rotary_Encoder.3dshapes` at all, and a
`Connector_USB.3dshapes` of twelve files with no HRO part among them. The
footprint libraries name model paths the 3D library does not contain, so
reinstalling KiCad cannot help.

## Provenance and licensing

The geometry comes from LCSC, via the EasyEDA component API, and is **not
covered by this project's licence**. It is third-party data, kept here the
way `fixture/board` keeps pinside's output: committed so the board renders on
a fresh clone, attributed rather than relabelled. Each `.wrl` names its LCSC
part in a header comment. `tools/fetch3d.py` regenerates all three, so the
files are reproducible rather than opaque.

Taking the models from LCSC rather than a manufacturer has one concrete
advantage: the model is the one for the part number in the BOM, so the
viewer shows what the assembler will fit. That paid for itself immediately.
SW3's model arrived as a **Bourns PEC11R**, which is what `C7275025` is, and
its mounting lugs sit 13.99 mm apart against the Alps EC11E footprint's
11.20 mm. The part does not fit the footprint it was placed on; tracked as
`A32`.

## Two things to know

**The placement is checked, not eyeballed.** An OBJ carries no datum, so
each transform is verified by locating the model's pins in the mesh and
requiring them to land on the footprint's pads within 0.25 mm. It earns its
keep: the first USB-C offset, estimated from the bounding box, put all four
shield posts 0.745 mm out and the check rejected it.

**VRML is rendered but not exported.** KiCad 10 reads STEP, IGES, VRML and
X3D, and LCSC serves Wavefront OBJ, so these are converted to VRML. The
3D viewer shows them; `kicad-cli pcb export step` does not. A board STEP
export will be missing these nine footprints, which matters only if the
enclosure is ever fitted against an imported board model rather than against
the numbers in `hardware/enclosure/safekiln-enclosure.scad`.
