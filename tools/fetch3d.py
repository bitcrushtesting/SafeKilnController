#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Bitcrush Testing
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the 3D models KiCad does not ship, and convert them for KiCad.

    tools/fetch3d.py --self-test       check the maths, no network
    tools/fetch3d.py                   fetch and write hardware/3dmodels/*.wrl
    tools/fetch3d.py --check           verify the committed .wrl files

===========================================================================
WHY THIS EXISTS
===========================================================================
Four footprints on this board name a 3D model that does not exist, and not
because anything local is broken: KiCad's footprint libraries reference model
files its 3D library has never shipped. Upstream kicad-packages3D has
TerminalBlock_Altech, _Ningbo-Kagnex, _Phoenix and _Wuerth but no plain
TerminalBlock.3dshapes, no Rotary_Encoder.3dshapes at all, and its
Connector_USB.3dshapes holds twelve files, none of them the HRO part. So
re-running the KiCad installer cannot fix it and neither can waiting.

The models here come from LCSC instead, which has one advantage over a
generic manufacturer model: it is the model for the exact part number in the
BOM, so what the 3D viewer shows is what the assembler will fit. That
mattered immediately. The encoder model arrived as a Bourns PEC11R, which is
what C7275025 is, and it does not fit the Alps EC11E footprint it was placed
on (see MOUNT_MISMATCH below).

LCSC serves Wavefront OBJ. KiCad 10 reads STEP, IGES, VRML and X3D, so the
models are converted to VRML. The consequence worth knowing: VRML bodies are
rendered but ignored by `kicad-cli pcb export step`, so a board STEP export
will not contain these four parts.

===========================================================================
WHY THE TRANSFORMS ARE A TABLE AND NOT A GUESS
===========================================================================
An OBJ carries no datum. Dropping one into KiCad unaltered puts it near the
origin at some arbitrary height, and "looks about right in the viewer" is not
a check that can fail. So each transform below is derived from the geometry
and then VERIFIED against the footprint it has to sit on: the model's pins
are located in the mesh and must land on that footprint's pads to
PIN_TOL_MM. A wrong rotation, a wrong flip or a missed offset moves the pins
and the check fails.

The Y flip is not a preference. LCSC's OBJ is in footprint coordinates,
which run Y-down like the schematic, while KiCad's 3D space is Y-up. The
USB-C part proves the direction on its own: its sixteen pads sit in one row
at footprint y = -4.045, and only the flip puts the model's contact row
there. Without it the body lands 8 mm off the footprint entirely.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import sys
import urllib.error
import urllib.request

# KiCad's VRML loader treats one unit as 0.1 inch.
WRL_UNIT_MM = 25.4 / 10.0

# How close a model's pins must land to the footprint's pads.
PIN_TOL_MM = 0.25

# How far a surface-mount body's underside may sit from the board plane.
# Loose enough for a lead that dips below its own seating plane, which the
# buzzer's does by 0.2, and tight enough to catch a part placed at the wrong
# datum, which in practice means half a millimetre or more: the one real
# error this check has caught so far was 0.745.
SEAT_TOL_MM = 0.3

HERE = pathlib.Path(__file__).resolve().parent.parent
OUT_DIR = HERE / "hardware" / "3dmodels"
KICAD_FP = pathlib.Path(
    "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
)
# Footprints this project owns, named with this prefix in the table below.
PROJECT_FP = HERE / "hardware"

API = "https://easyeda.com/api/products/{part}/components?version=6.4.19.5"
MODEL = "https://easyeda.com/analyzer/api/3dmodel/{uuid}"
# Without a browser's headers the CDN in front of this answers 403.
HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
    ),
    "Accept": "application/json,text/plain,*/*",
    "Referer": "https://easyeda.com/",
}


class Part:
    """One model: where it comes from, how it is placed, and how to prove it.

    rotate_deg turns the model about Z after the Y flip and before the
    offset, because that is the order the pin check below assumes.
    """

    def __init__(self, name, lcsc, refs, footprint, rotate_deg, offset,
                 pins, pin_pick, notes=""):
        self.name = name
        self.lcsc = lcsc
        self.refs = refs
        self.footprint = footprint
        self.rotate_deg = rotate_deg
        self.offset = offset
        self.pins = pins              # footprint pad names the pins must hit
        self.pin_pick = pin_pick      # (z_below, cluster_axis) locates pins
        self.notes = notes


PARTS = [
    Part(
        name="terminalblock_kf301_5.0_2p",
        lcsc="C474881",
        refs="J1 J3 J6 J7 J8 J9 J10",
        footprint="TerminalBlock.pretty/"
                  "TerminalBlock_MaiXu_MX126-5.0-02P_1x02_P5.00mm.kicad_mod",
        rotate_deg=0,
        # +2.5 puts the model's pin pair, which straddles its origin at
        # x = +/-2.5, onto pads at x = 0 and x = 5. +4.95 is the underside of
        # the body, and lands the pin tips 3.50 mm below the board, which is
        # the KF301's stated pin length.
        offset=(2.5, -0.15, 4.95),
        pins=["1", "2"],
        pin_pick=(-7.9, "x"),
        notes="Footprint is the MaiXu MX126; the ordered part is a Cixi Kefa "
              "KF301. Same 5.00 mm pitch and the pins land exactly, but the "
              "bodies differ by about 0.2 mm in depth.",
    ),
    Part(
        name="rotaryencoder_pec11r_4215k",
        lcsc="C7275025",
        refs="SW3",
        footprint="Rotary_Encoder.pretty/"
                  "RotaryEncoder_Alps_EC11E-Switch_Vertical_H20mm.kicad_mod",
        # The model's two terminal rows are separated along Y and the
        # footprint's along X.
        rotate_deg=90,
        # +4.70 is the lowest plane carrying the full 12.20 x 13.20 body
        # section, so the body's underside sits on the board. It puts the
        # shaft tip 20.8 mm up, against the footprint's own H20mm.
        offset=(7.43, -2.5, 4.70),
        pins=["A", "B", "C", "S1", "S2"],
        pin_pick=(-8.0, "x"),
        notes="MOUNT_MISMATCH: the five terminals land exactly, but this "
              "part's mounting lugs are 13.99 mm apart and the footprint's "
              "MP holes are 11.20 mm apart. See tasklist A32.",
    ),
    Part(
        name="sw_push_xkb_ts-1187a",
        lcsc="C318884",
        refs="SW1 SW2",
        footprint="Button_Switch_SMD.pretty/"
                  "SW_Push_1P1T_XKB_TS-1187A.kicad_mod",
        rotate_deg=0,
        # Identity. The part is symmetric about its own origin, the footprint
        # is symmetric about its pad centroid, and the mesh already starts at
        # the board plane: z runs -0.05 to 1.50, which is this variant's
        # 1.5 mm height. Nothing to move, which the centre check below
        # confirms rather than assumes.
        offset=(0.0, 0.0, 0.0),
        pins=[],
        # No through-hole geometry to cluster, so this one is checked on its
        # centre and its seating plane instead. See check_smd().
        pin_pick=None,
        notes="Gull-wing leads reach x = +/-3.25, so a 6.5 mm lead span, "
              "which is what LCSC's package name states.",
    ),
    Part(
        name="buzzer_fst-9650b-5v",
        lcsc="C391033",
        refs="BZ1",
        footprint="safekiln.pretty/"
                  "Buzzer_FUET_FST-9650B-5V_9.6x9.6mm.kicad_mod",
        rotate_deg=0,
        # Identity, and this one is the reason the footprint can be trusted.
        # The land pattern was taken from LCSC's own footprint rather than
        # read off a datasheet, because the PDF's text is CID-encoded and
        # does not extract. The mesh is the independent check: its leads run
        # x 3.05 to 5.00 by y +/-0.75, a 10.00 mm lead span exactly as the
        # package name states, and the drawn pads cover 3.25 to 5.75 by
        # +/-0.90. Body 9.6 mm in Y, seated at z = -0.2.
        offset=(0.0, 0.0, 0.0),
        pins=[],
        pin_pick=None,
        notes="Pin 1 is positive and goes to +5 V; pin 2 is the low-side "
              "drain. Same polarity as the through-hole part it replaces.",
    ),
    Part(
        name="usbc_hro_type-c-31-m-12",
        lcsc="C165948",
        refs="J2",
        footprint="Connector_USB.pretty/"
                  "USB_C_Receptacle_HRO_TYPE-C-31-M-12.kicad_mod",
        rotate_deg=0,
        # Z is already referenced to the board: the body's base plane is at
        # z = 0 and only the shield posts go below it.
        #
        # -1.05 is the model's own origin landing on the near row of SH pads,
        # which sit at footprint y = +1.05. Estimating this offset from the
        # body's bounding box instead gave -0.305, and the pin check rejected
        # it: all four shield posts came out 0.745 mm adrift, by the same
        # amount in the same direction, which is what a plain datum error
        # looks like.
        offset=(0.0, -1.05, 0.0),
        pins=["SH"],
        pin_pick=(-0.05, "x"),
        notes="The 16 signal pads are a single row, so this part is what "
              "fixes the Y-flip direction for every model here.",
    ),
]


# --------------------------------------------------------------------------
# geometry
# --------------------------------------------------------------------------

def place(v, rotate_deg, offset):
    """Model coordinate -> KiCad 3D coordinate, in mm.

    Y is flipped first, then the rotation about Z, then the offset.
    """
    x, y, z = v
    y = -y
    if rotate_deg:
        a = math.radians(rotate_deg)
        ca, sa = math.cos(a), math.sin(a)
        x, y = x * ca - y * sa, x * sa + y * ca
    return (x + offset[0], y + offset[1], z + offset[2])


def cluster(vals, gap):
    """Group sorted values whose neighbours are closer together than gap."""
    out = []
    for v in sorted(vals):
        if out and v - out[-1][-1] < gap:
            out[-1].append(v)
        else:
            out.append([v])
    return [(g[0] + g[-1]) / 2.0 for g in out]


def parse_obj(text):
    """Return (vertices, faces, materials) from LCSC's OBJ+MTL in one file."""
    verts, faces, mats, cur = [], [], {}, None
    mat_name = None
    for line in text.splitlines():
        if line.startswith("v "):
            verts.append(tuple(float(f) for f in line.split()[1:4]))
        elif line.startswith("f "):
            idx = []
            for tok in line.split()[1:]:
                n = int(tok.split("/")[0])
                idx.append(n - 1 if n > 0 else len(verts) + n)
            faces.append((cur, idx))
        elif line.startswith("usemtl"):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith("newmtl"):
            mat_name = line.split(None, 1)[1].strip()
            mats[mat_name] = {}
        elif mat_name and line[:2] in ("Kd", "Ka", "Ks") and line[2:3] == " ":
            mats[mat_name][line[:2]] = [float(f) for f in line.split()[1:4]]
        elif mat_name and line.startswith("d "):
            mats[mat_name]["d"] = float(line.split()[1])
    return verts, faces, mats


def footprint_pads(path):
    """Pad name -> list of (x, y) in footprint coordinates."""
    text = path.read_text()
    pads = {}
    for m in re.finditer(
        r'\(pad "([^"]*)" \w+ \w+\s*\n\s*\(at ([-\d.]+) ([-\d.]+)', text
    ):
        pads.setdefault(m.group(1), []).append(
            (float(m.group(2)), float(m.group(3)))
        )
    return pads


def model_pin_xy(verts, part):
    """Where the model's pins are, after placement, as (x, y) centres.

    The pins are the geometry below z_below. They are separated along one
    axis, so clustering that axis gives one centre per pin.
    """
    z_below, axis = part.pin_pick
    low = [v for v in verts if v[2] < z_below]
    if not low:
        return []
    placed = [place(v, part.rotate_deg, part.offset) for v in low]
    i = 0 if axis == "x" else 1
    j = 1 - i
    centres = cluster([round(p[i], 2) for p in placed], 0.6)
    out = []
    for c in centres:
        near = [p for p in placed if abs(p[i] - c) < 0.6]
        other = cluster([round(p[j], 2) for p in near], 0.9)
        for o in other:
            out.append((c, o) if i == 0 else (o, c))
    return out


def check_smd(verts, part, pads):
    """Centre and seating-plane check, for a part with no pins below board.

    A surface-mount part's leads sit in the board plane, so there is no
    geometry below it to cluster and locate. What can still be checked is
    that the body is centred where the pads are and that it rests on the
    board rather than floating or sunk: both are what a bad offset breaks.
    """
    placed = [place(v, part.rotate_deg, part.offset) for v in verts]
    all_pads = [(x, -y) for pts in pads.values() for (x, y) in pts]
    if not all_pads:
        return [f"{part.name}: the footprint has no pads to centre on"]
    want = (sum(p[0] for p in all_pads) / len(all_pads),
            sum(p[1] for p in all_pads) / len(all_pads))
    got = ((min(p[0] for p in placed) + max(p[0] for p in placed)) / 2.0,
           (min(p[1] for p in placed) + max(p[1] for p in placed)) / 2.0)
    errs = []
    d = math.dist(want, got)
    if d > PIN_TOL_MM:
        errs.append(
            f"{part.name}: body centre ({got[0]:.2f},{got[1]:.2f}) is "
            f"{d:.2f} mm from the pad centroid "
            f"({want[0]:.2f},{want[1]:.2f})"
        )
    base = min(p[2] for p in placed)
    if abs(base) > SEAT_TOL_MM:
        errs.append(
            f"{part.name}: the mesh's lowest point is z={base:.2f}, so the "
            f"part {'floats above' if base > 0 else 'sinks into'} the board"
        )
    return errs


def check_pins(verts, part, pads):
    """Every named pad must have a model pin on it. Returns a list of errors."""
    if part.pin_pick is None:
        return check_smd(verts, part, pads)
    want = []
    for name in part.pins:
        for (px, py) in pads.get(name, []):
            want.append((name, px, -py))      # footprint Y-down -> 3D Y-up
    got = model_pin_xy(verts, part)
    errs = []
    for name, px, py in want:
        if not got:
            errs.append(f"{part.name}: no pin geometry found in the mesh")
            break
        d, best = min(
            ((math.dist((px, py), g), g) for g in got), key=lambda t: t[0]
        )
        if d > PIN_TOL_MM:
            errs.append(
                f"{part.name}: pad {name} at ({px:.2f},{py:.2f}) has no pin "
                f"within {PIN_TOL_MM} mm; nearest mesh pin "
                f"({best[0]:.2f},{best[1]:.2f}) is {d:.2f} mm away"
            )
    return errs


def to_wrl(verts, faces, mats, part):
    """Emit VRML 2.0, one Shape per material, in KiCad's 0.1 inch units."""
    groups = {}
    for mat, idx in faces:
        groups.setdefault(mat, []).append(idx)
    placed = [place(v, part.rotate_deg, part.offset) for v in verts]
    scaled = [(x / WRL_UNIT_MM, y / WRL_UNIT_MM, z / WRL_UNIT_MM)
              for x, y, z in placed]

    out = ["#VRML V2.0 utf8",
           f"# {part.name}  LCSC {part.lcsc}  for {part.refs}",
           "# Generated by tools/fetch3d.py. Do not edit by hand.",
           f"# Units: 1 = {WRL_UNIT_MM} mm, as KiCad's VRML loader expects.",
           ""]
    for mat, polys in groups.items():
        spec = mats.get(mat, {})
        kd = spec.get("Kd", [0.6, 0.6, 0.6])
        ks = spec.get("Ks", [0.2, 0.2, 0.2])
        used = sorted({i for p in polys for i in p})
        remap = {old: new for new, old in enumerate(used)}
        out.append("Shape {")
        out.append("  appearance Appearance { material Material {")
        out.append("    diffuseColor %.4f %.4f %.4f" % tuple(kd))
        out.append("    specularColor %.4f %.4f %.4f" % tuple(ks))
        if spec.get("d", 1.0) < 1.0:
            out.append("    transparency %.3f" % (1.0 - spec["d"]))
        out.append("  } }")
        out.append("  geometry IndexedFaceSet {")
        out.append("    coord Coordinate { point [")
        out.append(",\n".join(
            "      %.5f %.5f %.5f" % scaled[i] for i in used))
        out.append("    ] }")
        out.append("    coordIndex [")
        out.append(",\n".join(
            "      " + " ".join(str(remap[i]) for i in p) + " -1"
            for p in polys))
        out.append("    ]")
        out.append("    solid FALSE")
        out.append("  }")
        out.append("}")
        out.append("")
    return "\n".join(out)


# --------------------------------------------------------------------------
# network
# --------------------------------------------------------------------------

def get(url, binary=False):
    req = urllib.request.Request(url, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=90) as r:
        data = r.read()
    return data if binary else data.decode("utf-8", "replace")


def model_uuid(lcsc):
    doc = json.loads(get(API.format(part=lcsc)))
    if not doc.get("success"):
        raise RuntimeError(f"{lcsc}: API returned success=false")
    shapes = ((doc["result"].get("packageDetail") or {})
              .get("dataStr") or {}).get("shape") or []
    for s in shapes:
        if s.startswith("SVGNODE"):
            attrs = json.loads(s.split("~", 1)[1]).get("attrs", {})
            if attrs.get("uuid"):
                return attrs["uuid"], doc["result"].get("title", "?")
    raise RuntimeError(f"{lcsc}: no 3D model attached to the package")


# --------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------

def self_test():
    checks, failed = 0, 0

    def ok(cond, what):
        nonlocal checks, failed
        checks += 1
        if not cond:
            failed += 1
            print(f"  FAIL: {what}")

    # place(): the Y flip, the rotation and the offset, each on its own.
    ok(place((1, 2, 3), 0, (0, 0, 0)) == (1, -2, 3), "Y is flipped")
    ok(place((0, 0, 0), 0, (1, 2, 3)) == (1, 2, 3), "offset applies")
    x, y, _ = place((1, 0, 0), 90, (0, 0, 0))
    ok(abs(x) < 1e-9 and abs(y - 1) < 1e-9, "+90 deg takes +x to +y")
    x, y, _ = place((0, -1, 0), 90, (0, 0, 0))   # flips to +1, then rotates
    ok(abs(x + 1) < 1e-9 and abs(y) < 1e-9, "+90 deg takes +y to -x")

    # A wrong transform has to be caught, or the check is decoration.
    verts = [(-2.5, -0.15, -8.0), (2.5, -0.15, -8.0)]
    good = PARTS[0]
    pads = {"1": [(0.0, 0.0)], "2": [(5.0, 0.0)]}
    ok(check_pins(verts, good, pads) == [], "the real KF301 transform passes")

    bad = Part("bad", "x", "x", "x", 0, (0.0, -0.15, 4.95), ["1", "2"],
               (-7.9, "x"))
    ok(check_pins(verts, bad, pads) != [], "a missing X offset is caught")

    bad2 = Part("bad2", "x", "x", "x", 90, (2.5, -0.15, 4.95), ["1", "2"],
                (-7.9, "x"))
    ok(check_pins(verts, bad2, pads) != [], "a spurious rotation is caught")

    # The Y flip must matter: a part whose pads are off-axis has to fail
    # without it. This is the USB-C's single pad row, in miniature.
    v2 = [(-1.0, -4.045, -0.1), (1.0, -4.045, -0.1)]
    p2 = Part("p2", "x", "x", "x", 0, (0.0, 0.0, 0.0), ["SH"], (-0.05, "x"))
    pads2 = {"SH": [(-1.0, -4.045), (1.0, -4.045)]}
    ok(check_pins(v2, p2, pads2) == [], "flip puts a -Y pad row on the mesh")
    noflip = [(x, -y, z) for x, y, z in v2]
    ok(check_pins(noflip, p2, pads2) != [], "without the flip it fails")

    # check_smd(): the centre and the seating plane, and both must be able
    # to fail. A 5.1 mm body on pads at +/-3, +/-1.875, like the TS-1187A.
    pads_sw = {"1": [(-3.0, -1.875), (3.0, -1.875)],
               "2": [(-3.0, 1.875), (3.0, 1.875)]}
    body = [(-2.55, -2.6, 0.0), (2.55, 2.6, 1.5)]
    smd = Part("smd", "x", "x", "x", 0, (0, 0, 0), [], None)
    ok(check_smd(body, smd, pads_sw) == [], "a centred seated body passes")
    off = Part("off", "x", "x", "x", 0, (1.0, 0, 0), [], None)
    ok(check_smd(body, off, pads_sw) != [], "an off-centre body is caught")
    up = Part("up", "x", "x", "x", 0, (0, 0, 0.5), [], None)
    ok(check_smd(body, up, pads_sw) != [], "a floating body is caught")
    sunk = Part("sunk", "x", "x", "x", 0, (0, 0, -0.5), [], None)
    ok(check_smd(body, sunk, pads_sw) != [], "a sunken body is caught")
    ok(check_pins(body, smd, pads_sw) == [],
       "check_pins routes a pin_pick of None to check_smd")

    # cluster()
    ok(cluster([0.0, 0.1, 5.0, 5.1], 0.6) == [0.05, 5.05], "cluster splits")
    ok(len(cluster([0.0, 0.3, 0.6], 0.6)) == 1, "cluster joins a chain")

    # parse_obj() on a minimal document, materials included.
    doc = ("v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl 1\nf 1 2 3\n"
           "newmtl 1\nKd 0.5 0.4 0.3\nd 0.5\n")
    verts, faces, mats = parse_obj(doc)
    ok(len(verts) == 3 and len(faces) == 1, "OBJ vertices and faces parse")
    ok(faces[0][0] == "1", "a face keeps its material")
    ok(mats["1"]["Kd"] == [0.5, 0.4, 0.3], "material colour parses")
    ok(mats["1"]["d"] == 0.5, "material transparency parses")

    # to_wrl(): scaling and a valid-looking document.
    part = Part("t", "C1", "R1", "f", 0, (0, 0, 0), [], (-1, "x"))
    text = to_wrl([(25.4, 0, 0), (0, 25.4, 0), (0, 0, 25.4)],
                  [("1", [0, 1, 2])], {"1": {"Kd": [1, 1, 1]}}, part)
    ok(text.startswith("#VRML V2.0 utf8"), "WRL has its header")
    ok("10.00000 0.00000 0.00000" in text, "25.4 mm becomes 10 units")
    ok(text.count("Shape {") == 1, "one Shape per material")
    ok("-1" in text.split("coordIndex")[1], "faces are -1 terminated")

    print(f"fetch3d self-test: {checks - failed}/{checks} checks passed")
    return 1 if failed else 0


# --------------------------------------------------------------------------

def run(check_only):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    rc = 0
    for part in PARTS:
        dest = OUT_DIR / f"{part.name}.wrl"
        root = (PROJECT_FP if part.footprint.startswith("safekiln.pretty/")
                else KICAD_FP)
        fp = root / part.footprint
        print(f"=== {part.name}  ({part.lcsc}, {part.refs})")

        if check_only:
            if not dest.exists():
                print(f"  MISSING {dest.relative_to(HERE)}")
                rc = 1
                continue
            print(f"  present, {dest.stat().st_size} bytes")
            continue

        try:
            uuid, title = model_uuid(part.lcsc)
            print(f"  LCSC says: {title}   model {uuid}")
            raw = get(MODEL.format(uuid=uuid))
        except (urllib.error.URLError, RuntimeError, OSError) as exc:
            print(f"  FETCH FAILED: {exc}")
            rc = 1
            continue

        verts, faces, mats = parse_obj(raw)
        print(f"  mesh: {len(verts)} vertices, {len(faces)} faces, "
              f"{len(mats)} materials")

        if fp.exists():
            errs = check_pins(verts, part, footprint_pads(fp))
            if errs:
                # Not fatal for the mounting-lug case, which is a board
                # problem rather than a transform problem, but it must be
                # said out loud either way.
                for e in errs:
                    print(f"  PIN CHECK: {e}")
                rc = 1
            elif part.pin_pick is None:
                print(f"  placement: body centred on the pad centroid "
                      f"within {PIN_TOL_MM} mm and seated within "
                      f"{SEAT_TOL_MM} mm of the board")
            else:
                print(f"  pin check: all {len(part.pins)} pads matched "
                      f"within {PIN_TOL_MM} mm")
        else:
            print(f"  pin check SKIPPED, no footprint at {fp}")
            rc = 1

        dest.write_text(to_wrl(verts, faces, mats, part))
        print(f"  wrote {dest.relative_to(HERE)} "
              f"({dest.stat().st_size} bytes)")
        if part.notes:
            print(f"  note: {part.notes}")
    return rc


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--check", action="store_true",
                    help="verify the committed files, no network")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    return run(args.check)


if __name__ == "__main__":
    sys.exit(main())
