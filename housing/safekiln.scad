// SPDX-FileCopyrightText: 2026 Bitcrush Testing
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Safe Kiln Controller -- enclosure
//
// Parametric two-part housing: a base holding the controller PCB and the
// mains switching relay in segregated bays, and a lid carrying the display
// and encoder.
//
// Requires BOSL2, vendored as a submodule at housing/libraries/BOSL2.
//   git submodule update --init --recursive
//
// Render:
//   openscad -D 'part="base"' -o base.stl safekiln.scad
//   openscad -D 'part="lid"'  -o lid.stl  safekiln.scad
//   openscad -D 'part="all"'  -o all.stl  safekiln.scad   (assembled preview)

include <libraries/BOSL2/std.scad>

/* [What to render] */
// base | lid | all
part = "all";

/* [PCB] ------------------------------------------------------------------ */
// Matches the KiCad board outline: 100 x 100 mm, M3 at 4.5 mm inset.
pcb_x          = 100;
pcb_y          = 100;
pcb_t          = 1.6;
pcb_hole_inset = 4.5;
pcb_hole_d     = 3.2;   // clearance hole in the board
pcb_gap        = 1.0;   // clearance around the board inside its bay

// Height of the tallest thing standing on the PCB. The EC11 encoder body and
// the 5.08 mm screw terminals set this; the shaft passes through the lid.
pcb_tall_part  = 24;

/* [Relay: Songle SLA-05VDC-SL-C, LCSC C87367] ---------------------------- */
// Plan size is from the LCSC package field (32 x 27.6 mm).
// TODO verify height against the datasheet before printing; 20.6 is the
// published figure for the SLA series but has not been measured here.
relay_l   = 32.0;
relay_w   = 27.6;
relay_h   = 20.6;
relay_gap = 1.5;    // clearance around the relay body

/* [Shell] ---------------------------------------------------------------- */
wall        = 2.4;   // side walls
floor_t     = 2.4;
lid_t       = 2.4;
standoff_h  = 8.0;   // clearance under the PCB for through-hole leads
corner_r    = 3.0;
boss_d      = 7.0;   // lid screw boss outer diameter
boss_hole_d = 2.5;   // pilot for an M3 self-tapping screw
insert_d    = 4.2;   // set >0 to bore for M3 heat-set inserts instead
insert_h    = 5.0;
use_inserts = true;

/* [Ventilation] ---------------------------------------------------------- */
// SWR-SAF-11 trips on enclosure over-temperature, so the box has to actually
// breathe. Slots are on the side walls, not the top, to keep kiln-room dust
// and debris from dropping straight in.
vent            = true;
vent_slot_w     = 3.0;
vent_slot_h     = 14.0;
vent_slot_pitch = 7.0;
vent_count      = 6;

/* [Panel cutouts] -------------------------------------------------------- */
// Positions are relative to the PCB origin (its lower-left corner) and are
// PLACEHOLDERS until the KiCad placement is done -- the board has no
// footprints on it yet, so these cannot be final. Set panel_cutouts=false to
// print a blank lid and drill later.
panel_cutouts = true;

oled_win_x  = 30;    // window for the 128x64 OLED
oled_win_y  = 60;
oled_win_w  = 27.0;  // active area of a typical 1.3" SSD1306/SH1106 module
oled_win_h  = 15.0;

enc_x       = 78;    // encoder shaft
enc_y       = 30;
enc_shaft_d = 7.5;   // 6 mm shaft + clearance for the threaded bushing
enc_nut_d   = 10.0;  // recess for the mounting nut
enc_nut_h   = 1.2;

// Sound port over the on-board buzzer (BZ1, TMB12A05, 12 mm dia). A buzzer
// sealed inside the box is heavily muffled, and SWR-SAF-20 depends on the operator
// actually hearing it, so the lid is perforated above it.
buzz_x        = 45;    // centre of BZ1, relative to the PCB origin
buzz_y        = 15;
buzz_port_d   = 11.0;  // diameter of the perforated area
buzz_hole_d   = 1.6;   // individual hole
buzz_hole_ring= 7;     // holes in the outer ring

usb_x       = 0;     // USB-C access on the -X wall, centred on this Y
usb_y       = 20;
usb_w       = 11.0;
usb_h       = 5.5;

/* [Cable entry] ---------------------------------------------------------- */
// Field wiring (power, thermocouples, SSR, contactor coil, buzzer, CT) leaves
// through one wall so it can be strain-relieved as a single loom.
cable_slot   = true;
cable_slot_w = 60;
cable_slot_h = 12;

/* ======================================================================== */
/* Derived                                                                  */
/* ======================================================================== */

pcb_bay_x = pcb_x + 2*pcb_gap;
pcb_bay_y = pcb_y + 2*pcb_gap;

relay_bay_x = relay_l + 2*relay_gap;
relay_bay_y = relay_w + 2*relay_gap;

// Barrier between the SELV control bay and the relay, whose terminals carry
// mains. The controller PCB itself is entirely SELV -- the only mains on this
// product is at the relay and inside the kiln.
barrier_t = wall;

inner_x = pcb_bay_x + barrier_t + relay_bay_x;
inner_y = max(pcb_bay_y, relay_bay_y);
inner_z = standoff_h + pcb_t + pcb_tall_part;

outer_x = inner_x + 2*wall;
outer_y = inner_y + 2*wall;
outer_z = floor_t + inner_z;

// Origin of the PCB (its lower-left corner) in base-local coordinates, where
// (0,0) is the centre of the outer footprint.
pcb_ox = -inner_x/2 + pcb_gap;
pcb_oy = -inner_y/2 + pcb_gap;

// The four PCB mounting points, as offsets from the PCB origin.
pcb_holes = [
    [pcb_hole_inset,          pcb_hole_inset],
    [pcb_x - pcb_hole_inset,  pcb_hole_inset],
    [pcb_hole_inset,          pcb_y - pcb_hole_inset],
    [pcb_x - pcb_hole_inset,  pcb_y - pcb_hole_inset],
];

// Lid screw bosses, inset from each corner of the inner cavity.
boss_inset = boss_d/2 + 0.6;
boss_pos = [
    [-inner_x/2 + boss_inset, -inner_y/2 + boss_inset],
    [ inner_x/2 - boss_inset, -inner_y/2 + boss_inset],
    [-inner_x/2 + boss_inset,  inner_y/2 - boss_inset],
    [ inner_x/2 - boss_inset,  inner_y/2 - boss_inset],
];

$fn = 48;

/* ======================================================================== */
/* Base                                                                     */
/* ======================================================================== */

module standoffs() {
    for (h = pcb_holes)
        translate([pcb_ox + h[0], pcb_oy + h[1], floor_t])
            difference() {
                cyl(h = standoff_h, d = pcb_hole_d + 3.2, anchor = BOTTOM);
                // Pilot for an M3 self-tapping screw into the standoff.
                down(0.01) cyl(h = standoff_h + 0.02, d = 2.5, anchor = BOTTOM);
            }
}

module lid_bosses() {
    for (p = boss_pos)
        translate([p[0], p[1], floor_t])
            difference() {
                cyl(h = inner_z, d = boss_d, anchor = BOTTOM);
                up(inner_z - (use_inserts ? insert_h : inner_z) + 0.01)
                    cyl(h = (use_inserts ? insert_h : inner_z) + 0.02,
                        d  = (use_inserts ? insert_d : boss_hole_d),
                        anchor = BOTTOM);
                // Always leave a through pilot so a longer screw can be used.
                down(0.01) cyl(h = inner_z + 0.02, d = boss_hole_d, anchor = BOTTOM);
            }
}

module relay_pocket() {
    // A shallow rib pocket that locates the relay body and stops it sliding.
    rx = inner_x/2 - relay_bay_x/2;
    translate([rx, 0, floor_t])
        difference() {
            cuboid([relay_bay_x, relay_bay_y, 3], anchor = BOTTOM);
            down(0.01)
                cuboid([relay_l + 0.6, relay_w + 0.6, 3.02], anchor = BOTTOM);
        }
}

module barrier_wall() {
    // Segregates the relay (mains) from the control bay (SELV).
    bx = -inner_x/2 + pcb_bay_x + barrier_t/2;
    translate([bx, 0, floor_t])
        cuboid([barrier_t, inner_y, inner_z], anchor = BOTTOM);
}

module vent_slots(y_face) {
    // Cut through a Y wall at +/- inner_y/2.
    span = (vent_count - 1) * vent_slot_pitch;
    for (i = [0 : vent_count - 1])
        translate([-span/2 + i*vent_slot_pitch,
                   y_face * (inner_y/2 + wall/2),
                   floor_t + inner_z/2])
            cuboid([vent_slot_w, wall * 3, vent_slot_h], anchor = CENTER,
                   rounding = vent_slot_w/2 - 0.01, edges = "Y");
}

module base() {
    difference() {
        union() {
            // Outer shell with an open top.
            difference() {
                cuboid([outer_x, outer_y, outer_z], anchor = BOTTOM,
                       rounding = corner_r, edges = "Z");
                up(floor_t)
                    cuboid([inner_x, inner_y, inner_z + 0.02], anchor = BOTTOM,
                           rounding = max(corner_r - wall, 0.1), edges = "Z");
            }
            standoffs();
            lid_bosses();
            relay_pocket();
            barrier_wall();
        }

        // Ventilation on both long walls.
        if (vent) { vent_slots(+1); vent_slots(-1); }

        // Field wiring exit on the -X wall.
        if (cable_slot)
            translate([-outer_x/2, 0, floor_t + standoff_h + pcb_t + cable_slot_h/2])
                cuboid([wall * 3, cable_slot_w, cable_slot_h], anchor = CENTER,
                       rounding = 2, edges = "X");

        // USB-C access, also on the -X wall.
        if (panel_cutouts)
            translate([-outer_x/2,
                       pcb_oy + usb_y,
                       floor_t + standoff_h + pcb_t + usb_h/2])
                cuboid([wall * 3, usb_w, usb_h], anchor = CENTER,
                       rounding = 1, edges = "X");
    }
}

/* ======================================================================== */
/* Lid                                                                      */
/* ======================================================================== */

module lid() {
    difference() {
        union() {
            cuboid([outer_x, outer_y, lid_t], anchor = BOTTOM,
                   rounding = corner_r, edges = "Z");
            // Lip that drops into the cavity and locates the lid.
            up(lid_t - 0.01)
                cuboid([inner_x - 0.4, inner_y - 0.4, 3], anchor = BOTTOM,
                       rounding = max(corner_r - wall, 0.1), edges = "Z");
        }

        // Screw holes into the base bosses.
        for (p = boss_pos)
            translate([p[0], p[1], -0.01]) {
                cyl(h = lid_t + 3.02, d = 3.4, anchor = BOTTOM);
                // Countersink.
                up(lid_t) cyl(h = 2, d1 = 3.4, d2 = 6.4, anchor = TOP);
            }

        // Clear the lip where the bosses are.
        for (p = boss_pos)
            translate([p[0], p[1], lid_t - 0.01])
                cyl(h = 3.02, d = boss_d + 0.6, anchor = BOTTOM);

        if (panel_cutouts) {
            // OLED window.
            translate([pcb_ox + oled_win_x, pcb_oy + oled_win_y, -0.01])
                cuboid([oled_win_w, oled_win_h, lid_t + 3.02], anchor = BOTTOM,
                       rounding = 1, edges = "Z");

            // Encoder shaft, with a recess for its nut on the outside.
            translate([pcb_ox + enc_x, pcb_oy + enc_y, -0.01])
                cyl(h = lid_t + 3.02, d = enc_shaft_d, anchor = BOTTOM);
            translate([pcb_ox + enc_x, pcb_oy + enc_y, lid_t - enc_nut_h])
                cyl(h = enc_nut_h + 0.01, d = enc_nut_d, anchor = BOTTOM);

            // Buzzer sound port: a centre hole plus a ring, which lets the
            // sound out without leaving an opening big enough to drop debris
            // straight onto the board.
            translate([pcb_ox + buzz_x, pcb_oy + buzz_y, -0.01]) {
                cyl(h = lid_t + 3.02, d = buzz_hole_d, anchor = BOTTOM);
                for (i = [0 : buzz_hole_ring - 1])
                    rotate([0, 0, i * 360 / buzz_hole_ring])
                        right(buzz_port_d/2 - buzz_hole_d)
                            cyl(h = lid_t + 3.02, d = buzz_hole_d, anchor = BOTTOM);
            }
        }
    }
}

/* ======================================================================== */

if (part == "base")      base();
else if (part == "lid")  lid();
else {
    base();
    // Exploded preview.
    up(outer_z + 18) lid();
    // PCB stand-in, for checking clearances.
    %translate([pcb_ox, pcb_oy, floor_t + standoff_h])
        cube([pcb_x, pcb_y, pcb_t]);
    // Relay stand-in.
    %translate([inner_x/2 - relay_bay_x/2, 0, floor_t + 3])
        cuboid([relay_l, relay_w, relay_h], anchor = BOTTOM);
}
