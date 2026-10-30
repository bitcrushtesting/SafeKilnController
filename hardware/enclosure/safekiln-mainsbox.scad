// SPDX-FileCopyrightText: 2026 Bitcrush Testing
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wall-mount mains box for the Safe Kiln Controller: the controller board, its
// 12 V supply, the safety contactor and the SSR, in one printed box.
//
//   openscad -D part=\"box\"  -o mainsbox.stl  safekiln-mainsbox.scad
//   openscad -D part=\"lid\"  -o mainslid.stl  safekiln-mainsbox.scad
//   openscad -D part=\"knob\" -o stemknob.stl  safekiln-mainsbox.scad
//   openscad -D part=\"all\"  -o preview.png   safekiln-mainsbox.scad
//
// Or `./slice.sh mainsbox mainslid stemknob`.
//
// ===========================================================================
// WHAT GOES IN IT
// ===========================================================================
// For a single-phase kiln of up to 16 A at 230 V (3.6 kW):
//
//   -G1  Mean Well HDR-15-12        12 V 1.25 A DIN-rail supply, class II
//   -K1  Finder 22.32.0.012.4340    contactor, 2 NO, 25 A AC-1, 12 V coil
//   -V1  Crydom D2440               SSR, zero-cross, 40 A, 3 to 32 V DC in
//   -E1  heatsink, at most 1.5 K/W  outside the box, fins vertical
//   -F1  5 x 20 mm fuse terminal    T 1 A, feeds -G1 only
//   -X1 to -X6                      L, N, PE in; PE, N, L out to the kiln
//
// The contactor breaks L and N together. A kiln on a Schuko plug can have the
// two swapped by turning the plug over, and a one-pole break would then open
// the neutral and leave the elements live.
//
// ===========================================================================
// HOW IT IS LAID OUT
// ===========================================================================
// Seen from the front with the lid off, X to the right, Y up the wall, Z out
// of the wall towards the operator. Two bays, side by side, with a barrier
// wall between them that runs from the back plate to the lid:
//
//   SELV bay, left: the controller board, standing on edge as it does in the
//     panel enclosure, so the encoder shaft, the USB socket and the display
//     come through the LID, which is the operator's face. The board is
//     screwed to the left wall, not to the barrier, so there is no screw hole
//     in the one wall that separates the two voltages.
//
//   Mains bay, right: a DIN rail across the back plate carrying -G1 and -K1
//     next to the barrier, where their 12 V wiring crosses, and the terminals
//     towards the right. -V1 sits in an aperture in the right wall with its
//     base on -E1 outside.
//
// The 12 V wiring (-G1 out, -K1 coil, -V1 input, the CT secondary) crosses
// the barrier through one grommet, low down, near -G1's output terminals.
//
// ===========================================================================
// WHY THE SSR IS ON AN OUTSIDE HEATSINK
// ===========================================================================
// A D2440 drops about 1.2 W per amp, about 20 W at 16 A. Inside a closed
// plastic box of this size, 20 W raises the air by tens of kelvin, and
// SWR-SAF-11 trips the controller on enclosure temperature. So the SSR's base
// plate goes through the wall onto a heatsink outside, and what stays inside
// is the coil, the supply's losses and the contacts: a few watts.
//
// The SSR sits high in the box and towards the lid, in front of the DIN
// devices rather than beside them, so it needs no width of its own: Z range
// clear of every device front, Y range clear of every terminal screw. That is
// asserted below rather than hoped for.
//
// ===========================================================================
// WHAT IS NOT YET KNOWN
// ===========================================================================
// The DIN devices' outline sizes are from catalogues, not from the parts in
// hand; they set the rail length and the preview, nothing that is printed.
// The values that do reach the print are marked CHECK: the SSR and heatsink
// footprints, the encoder shaft's reach and where the shaft and the USB socket
// sit across the board's thickness.
//
// PRINTED PLASTIC IS NOT A MAINS ENCLOSURE BY DEFAULT. A part that holds live
// terminals has to pass the glow-wire test of EN 60695-2-11 (or be UL 94 V-0),
// and PLA, PETG and ordinary ASA do not. Print this in a flame-retardant grade
// (PC-FR, ASA-FR, PC/ABS V-0) for a real installation, or use it to check the
// layout and transfer the layout to a commercial box. See README.md.

use <safekiln-enclosure.scad>   // rrect(), boss(), speaker_grid(), knob()

// --------------------------------------------------------------------------
// what to render
// --------------------------------------------------------------------------
part = "all";          // "box" | "lid" | "knob" | "open" | "all"
$fn  = 48;

// --------------------------------------------------------------------------
// the board, as in safekiln-enclosure.scad (both are read from the PCB)
// --------------------------------------------------------------------------
pcb_w = 100.0;
pcb_h =  70.0;
pcb_t =   1.6;
pcb_hole_inset = 4.0;  // the HWR-24 proposal, the same as the panel box

h_terminal = 11.0;
h_encoder  = 14.0;
h_below    =  3.5;     // measured, see B6 in safekiln-enclosure.scad

enc_y  = 55.75;        // SW3
usb_y  = 23.90;        // J2, USB-C
dcin_y = 11.10;        // J1, +12 V in, wired from -G1 inside the box

// Where the encoder shaft and the USB-C socket sit across the board's
// thickness, from its mid-plane towards the component side. Zero is what the
// panel enclosure assumes: an encoder and a socket that straddle the edge.
enc_axis_off = 0.0;    // CHECK
usb_axis_off = 0.0;    // CHECK

// How far the encoder shaft's tip stands beyond the board's left edge. The
// board sits further behind the lid than behind the panel bezel, because J1's
// wire enters from the front and needs room for a ferrule and a bend, so the
// knob reaches in to the shaft on a stem rather than the shaft reaching out.
enc_shaft_tip = 15.0;  // CHECK on the encoder fitted
knob_engage   =  8.0;  // how much of the shaft the knob's bore takes
stem_d        = 11.0;

// --------------------------------------------------------------------------
// what goes on the rail
// --------------------------------------------------------------------------
// [designation, [width along the rail, height, depth from the rail's back],
//  gap before it, preview colour]. Catalogue outlines: CHECK against the
// parts bought, but only the rail length and the preview depend on them.
psu   = [17.5, 90.0, 54.5];   // -G1 Mean Well HDR-15-12
k1    = [17.5, 85.0, 64.0];   // -K1 Finder 22.32
term  = [ 6.2, 47.5, 47.7];   // -X1..-X6 Phoenix Contact UT 4 / UT 4-PE
fuse  = [ 8.2, 75.0, 57.0];   // -F1 5 x 20 mm fuse terminal
endbr = [ 9.5, 46.0, 35.0];   // end bracket

devs = [
    ["-G1",    psu,   0.0, "white"],
    ["-K1",    k1,    1.0, "lightgray"],
    ["end",    endbr, 4.0, "dimgray"],
    ["-F1",    fuse,  0.0, "gray"],
    ["-X1 L",  term,  0.0, "gray"],
    ["-X2 N",  term,  0.0, "royalblue"],
    ["-X3 PE", term,  0.0, "yellowgreen"],
    ["-X4 PE", term,  0.0, "yellowgreen"],
    ["-X5 N",  term,  0.0, "royalblue"],
    ["-X6 L",  term,  0.0, "gray"],
    ["end",    endbr, 0.0, "dimgray"],
];

rail        = [35.0, 7.5];    // TS 35 x 7.5
rail_boss_h =  5.0;
rail_end    =  6.0;           // rail beyond the outer devices, for its screws
rail_gap    =  6.0;           // barrier to the rail's left end

// -V1 and -E1. The SSR's long axis runs along Z, so the aperture's printed
// bridge is its short side.
ssr = [57.2, 44.8, 30.0];     // CHECK: along Z, along Y, into the bay
hs  = [100.0, 90.0, 40.0];    // CHECK: along Y, along Z, fin depth
hs_screw_y = 15.0;            // -E1 screws, either side of the SSR's centre
hs_screw_d = 4.5;             // M4 clearance; tap the heatsink M4

// --------------------------------------------------------------------------
// the shell
// --------------------------------------------------------------------------
wall      = 2.4;
back_t    = 3.0;
// Thicker than the outer walls: it is the insulation between the bays, and a
// printed wall's weakest point is a void between two perimeters.
barrier_t = 3.0;
corner_r  = 3.0;

lid_t = 3.0;
lip_h = 4.0;           // the lid's skirt, and its ribs either side of the barrier
lip_t = 1.6;
fit   = 0.3;

boss_d   = 8.0;
insert_d = 4.2;        // M3 heat-set
insert_h = 5.5;

ear_t = 4.0;           // wall-mounting ears, flush with the back
ear_d = 12.0;
ear_hole_d = 5.0;

// The SELV bay.
board_off   = 14.0;    // inner left wall to the board's back face
clear_front = 15.0;    // board's left edge to the lid: J1's wire and ferrule
clear_rear  = 20.0;    // terminal blocks to the back plate: bend radius
clear_top   = 15.0;

oled_pcb       = [27.0, 27.3];  // CHECK, as in the panel enclosure
oled_window    = [25.0, 14.5];  // CHECK
oled_window_off_y = 3.0;        // CHECK
oled_hole_grid = [23.0, 23.5];  // CHECK
oled_boss_h    =  4.0;
oled_lane      = oled_pcb[0] + 4.0;

usb_duct = [7.5, 13.0];  // across the board, along its edge: a USB-C overmould

// The mains bay.
wire_below = 40.0;     // under the rail devices: glands and the field cables
wire_above = 60.0;     // over them: the internal wiring, and the SSR

// --------------------------------------------------------------------------
// derived
// --------------------------------------------------------------------------
function sum(v, n) = n <= 0 ? 0 : sum(v, n - 1) + v[n - 1];
dev_pitch = [for (d = devs) d[2] + d[1][0]];
dev_x     = [for (i = [0 : len(devs) - 1]) sum(dev_pitch, i) + devs[i][2]];
rail_len  = sum(dev_pitch, len(devs)) + 2 * rail_end;

stack   = pcb_t + max(h_terminal, h_encoder) + 3.0;
selv_w  = board_off + stack + oled_lane;
mains_w = rail_gap + rail_len + 6.0;
in_h    = wire_below + max([for (d = devs) d[1][1]]) + wire_above;
in_d    = clear_front + pcb_w + clear_rear;

bar_x   = wall + selv_w;          // the barrier's SELV face
mains_x = bar_x + barrier_t;      // the mains bay's inner left
right_x = mains_x + mains_w;      // the right wall's inner face
outer_x = right_x + wall;
outer_y = in_h + 2 * wall;
top_in  = back_t + in_d;          // the rim, where the lid sits

rail_x  = mains_x + rail_gap;
rail_y  = wall + wire_below + max([for (d = devs) d[1][1]]) / 2;
dev_z0  = back_t + rail_boss_h;   // the rail's back, where device depth starts

// The board: its back face, mid-plane, bottom edge and operator edge.
bx   = wall + board_off;
bmid = bx + pcb_t / 2;
by   = wall + in_h - clear_top - pcb_h;
bzf  = top_in - clear_front;

knob_xy = [bmid + enc_axis_off, by + enc_y];
usb_xy  = [bmid + usb_axis_off, by + usb_y];
oled_xy = [bx + stack + oled_lane / 2, knob_xy[1]];

// -V1: high in the box so -E1 stays inside its outline, and towards the lid.
ssr_yc = outer_y - hs[0] / 2 - 2.0;
ssr_zc = top_in - 42.0;
hs_screws = [for (sy = [-1, 1], sz = [-1, 1])
                 [ssr_yc + sy * hs_screw_y, ssr_zc + sz * (ssr[0] / 2 + 7.0)]];

// The lid's screws: the four corners and both ends of the barrier.
bi = boss_d / 2;
boss_pos = [
    [wall + bi,              wall + bi],
    [wall + bi,              outer_y - wall - bi],
    [right_x - bi,           wall + bi],
    [right_x - bi,           outer_y - wall - bi],
    [bar_x + barrier_t / 2,  wall + bi],
    [bar_x + barrier_t / 2,  outer_y - wall - bi],
];

// Cable glands through the floor of each bay: [x from the bay's left, z, bore].
glands_selv  = [[16, 35, 16.3], [44, 35, 16.3], [30, 85, 16.3]];  // M16: TC1, TC2, lid
glands_mains = [[68, 45, 25.3], [100, 45, 25.3]];                 // M25: supply, kiln

grommet = [wall + 48.0, back_t + 27.0, 14.0];   // y, z, hole for a 14 mm grommet

// Wall-mounting ears, two each side. The right-hand pair stays below -E1,
// where a screwdriver can still reach them.
ears = [[0, 20], [0, outer_y - 20],
        [outer_x, 20], [outer_x, ssr_yc - hs[0] / 2 - ear_d]];

// --------------------------------------------------------------------------
// checks: the layout's claims, made by the model rather than by the comment
// --------------------------------------------------------------------------
// Every rail device whose width overlaps the SSR's must finish in front of it.
ssr_x0 = outer_x - ssr[2];
fronts_under_ssr = [for (i = [0 : len(devs) - 1])
    if (rail_x + rail_end + dev_x[i] + devs[i][1][0] > ssr_x0)
        dev_z0 + devs[i][1][2]];
assert(len(fronts_under_ssr) == 0
       || max(fronts_under_ssr) < ssr_zc - ssr[0] / 2,
       "the SSR collides with a DIN device in front of the rail");
// And the SSR stays above the terminal screws, which are driven from the lid.
assert(ssr_yc - ssr[1] / 2 > rail_y + term[1] / 2,
       "the SSR covers the terminals' screws");
assert(hs_screws[3][1] + hs_screw_d / 2 < top_in - 1,
       "a heatsink screw breaks out of the rim");
assert(ssr_yc + hs[0] / 2 <= outer_y,
       "the heatsink stands above the box");
// The board's components must stay clear of the display's lane.
assert(bx + stack <= oled_xy[0] - oled_pcb[0] / 2,
       "the board's stack reaches into the display lane");

// --------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------
// A horizontal hole that prints without support: a teardrop with its point
// towards +Z, cut off where a gland's flange or a grommet still covers it.
module teardrop2d(d) {
    r = d / 2;
    intersection() {
        hull() {
            circle(r = r);
            translate([0, r * sqrt(2)]) square(0.01, center = true);
        }
        translate([-r, -r]) square([2 * r, r + r * 1.15]);
    }
}
module hole_y(d, l) rotate([90, 0, 0]) linear_extrude(l, center = true) teardrop2d(d);
module hole_x(d, l) rotate([90, 0, 90]) linear_extrude(l, center = true) teardrop2d(d);

// --------------------------------------------------------------------------
// the box
// --------------------------------------------------------------------------
// Printed back-down, open face up. Every overhang is a wall or a teardrop,
// except the SSR aperture's top edge, a 46 mm bridge.
module board_standoff(x, y) {
    p = [wall, by + y, bzf - x];
    difference() {
        // A horizontal boss off a vertical wall, with a 45 degree gusset under
        // it so it does not print onto air.
        hull() {
            translate(p) rotate([0, 90, 0]) cylinder(d = 7.0, h = board_off);
            translate([wall - 0.01, p[1] - 3.5, p[2] - board_off - 3.5])
                cube([0.01, 7.0, board_off + 3.5]);
        }
        translate(p + [1, 0, 0]) rotate([0, 90, 0])
            cylinder(d = 2.6, h = board_off + 1);   // 2.6 taps M3
    }
}

module box() {
    difference() {
        union() {
            difference() {
                rrect([outer_x, outer_y], corner_r, top_in);
                translate([wall, wall, back_t])
                    rrect([outer_x - 2 * wall, outer_y - 2 * wall],
                          max(corner_r - wall, 0.5), in_d + 1);
            }

            // The barrier, back plate to rim. The lid's two ribs straddle
            // its top edge, so there is no straight gap over it.
            translate([bar_x, wall, back_t]) cube([barrier_t, in_h, in_d]);

            for (p = boss_pos)
                translate([p[0], p[1], back_t]) cylinder(d = boss_d, h = in_d);

            for (x = [pcb_hole_inset, pcb_w - pcb_hole_inset],
                 y = [pcb_hole_inset, pcb_h - pcb_hole_inset])
                board_standoff(x, y);

            for (x = [rail_x + 4.5, rail_x + rail_len - 4.5])
                translate([x, rail_y, back_t])
                    cylinder(d = 9.0, h = rail_boss_h);

            for (e = ears)
                hull() {
                    translate([e[0] + (e[0] == 0 ? -1 : 1) * (ear_d / 2 + 2),
                               e[1], 0])
                        cylinder(d = ear_d, h = ear_t);
                    translate([e[0] - 1, e[1] - ear_d / 2 - 2, 0])
                        cube([2, ear_d + 4, ear_t]);
                }
        }

        // --- the lid's heat-set inserts -----------------------------------
        for (p = boss_pos)
            translate([p[0], p[1], top_in - insert_h])
                cylinder(d = insert_d, h = insert_h + 1);

        // --- the rail's inserts, blind: nothing goes through to the wall --
        for (x = [rail_x + 4.5, rail_x + rail_len - 4.5])
            translate([x, rail_y, back_t + rail_boss_h - insert_h + 1])
                cylinder(d = insert_d, h = insert_h);

        for (e = ears)
            translate([e[0] + (e[0] == 0 ? -1 : 1) * (ear_d / 2 + 2), e[1], -1])
                cylinder(d = ear_hole_d, h = ear_t + 2);

        // --- cable glands, all in the floor of the box --------------------
        // Downwards, so water running down a cable drips off the gland
        // rather than following the cable in.
        for (g = glands_selv)
            translate([wall + g[0], wall / 2, g[1]]) hole_y(g[2], wall + 2);
        for (g = glands_mains)
            translate([mains_x + g[0], wall / 2, g[1]]) hole_y(g[2], wall + 2);

        // --- the 12 V crossing ---------------------------------------------
        translate([bar_x + barrier_t / 2, grommet[0], grommet[1]])
            hole_x(grommet[2], barrier_t + 2);

        // --- -V1 through the right wall, -E1's screws around it ----------
        translate([right_x - 1, ssr_yc - ssr[1] / 2 - 0.5, ssr_zc - ssr[0] / 2 - 0.5])
            cube([wall + 2, ssr[1] + 1.0, ssr[0] + 1.0]);
        for (s = hs_screws)
            translate([right_x + wall / 2, s[0], s[1]]) hole_x(hs_screw_d, wall + 2);

        // --- ventilation -----------------------------------------------------
        // Two millimetres wide everywhere, which keeps a finger and a wire
        // out (IP3X). In the floor near the lid, where the air comes in, and
        // high in both side walls, where it leaves: none in the top.
        for (x = [wall + 8 : 6 : bar_x - 8])
            translate([x - 1, -1, top_in - 34]) cube([2, wall + 2, 26]);
        for (x = [mains_x + 10 : 6 : right_x - 10])
            translate([x - 1, -1, top_in - 34]) cube([2, wall + 2, 26]);
        for (y = [outer_y - 26 : 5 : outer_y - 12], z = [30, 60, 90])
            translate([-1, y - 1, z]) cube([wall + 2, 2, 22]);
        for (y = [outer_y - 30 : 5 : outer_y - 15])
            translate([right_x - 1, y - 1, 12]) cube([wall + 2, 2, 30]);
    }
}

// --------------------------------------------------------------------------
// the lid
// --------------------------------------------------------------------------
// Modelled where it sits on the box and turned over for printing, so it
// prints face-down: the face the operator looks at is the one the bed made.
module lid_assembled() {
    sk = [outer_x - 2 * wall - 2 * fit, outer_y - 2 * wall - 2 * fit];
    difference() {
        union() {
            translate([0, 0, top_in]) rrect([outer_x, outer_y], corner_r, lid_t);

            // the skirt inside the walls
            translate([wall + fit, wall + fit, top_in - lip_h])
                difference() {
                    rrect(sk, 0.5, lip_h + 0.01);
                    translate([lip_t, lip_t, -0.1])
                        rrect(sk - [2 * lip_t, 2 * lip_t], 0.5, lip_h + 0.3);
                }

            // the two ribs either side of the barrier's top edge
            for (x = [bar_x - fit - lip_t, bar_x + barrier_t + fit])
                translate([x, wall, top_in - lip_h])
                    cube([lip_t, in_h, lip_h + 0.01]);

            // the display's standoffs
            for (sx = [-1, 1], sy = [-1, 1])
                translate([oled_xy[0] + sx * oled_hole_grid[0] / 2,
                           oled_xy[1] + sy * oled_hole_grid[1] / 2,
                           top_in - oled_boss_h])
                    boss(5.0, 1.7, oled_boss_h + 0.01);   // 1.7 taps M2

            // the USB duct, down to just in front of the socket, so a plug
            // reaches it through the lid
            translate([usb_xy[0], usb_xy[1], bzf + 1])
                linear_extrude(top_in - bzf - 1 + 0.01)
                    square(usb_duct + [3.2, 3.2], center = true);
        }

        // the skirt and the ribs stand clear of the bosses
        for (p = boss_pos)
            translate([p[0], p[1], top_in - lip_h - 0.1])
                cylinder(d = boss_d + 2 * fit, h = lip_h + 0.1);
        translate([bar_x - fit, wall - 1, top_in - lip_h - 0.1])
            cube([barrier_t + 2 * fit, in_h + 2, lip_h + 0.1]);

        // countersunk M3 into the inserts
        for (p = boss_pos)
            translate([p[0], p[1], top_in - 1]) {
                cylinder(d = 3.4, h = lid_t + 2);
                translate([0, 0, lid_t + 1 - 1.8])
                    cylinder(d1 = 3.4, d2 = 6.6, h = 1.81);
            }

        // the display window, chamfered on the outside
        translate([oled_xy[0], oled_xy[1] + oled_window_off_y, top_in - 0.1])
            union() {
                linear_extrude(lid_t + 0.2) square(oled_window, center = true);
                translate([0, 0, lid_t * 0.45])
                    linear_extrude(lid_t * 0.55 + 0.2)
                        square(oled_window + [2.0, 2.0], center = true);
            }

        // the knob's stem
        translate([knob_xy[0], knob_xy[1], top_in - 1])
            cylinder(d = stem_d + 1.0, h = lid_t + 2);

        // the USB duct's bore
        translate([usb_xy[0], usb_xy[1], bzf])
            linear_extrude(top_in + lid_t - bzf + 1)
                square(usb_duct, center = true);

        // the buzzer: BZ1 is inside the SELV bay, so its sound leaves through
        // the lid over that bay and never over the mains one
        translate([oled_xy[0], by + 15, top_in + lid_t / 2]) speaker_grid();

        // the warning, engraved into the face over the mains bay
        translate([mains_x + mains_w / 2, outer_y / 2, top_in + lid_t - 0.6])
            linear_extrude(1) {
                translate([0, 8]) bolt2d(18);
                text("230 V~", size = 11, halign = "center", valign = "center");
                translate([0, -14])
                    text("ISOLATE BEFORE OPENING", size = 4.5,
                         halign = "center", valign = "center");
            }
    }
}

module bolt2d(h) {
    s = h / 18;
    translate([-5.5 * s, 0])
        scale(s) polygon([[6, 18], [0, 8], [5, 8], [2, 0], [11, 11], [6, 11], [10, 18]]);
}

module lid() {
    translate([0, outer_y, top_in + lid_t]) rotate([180, 0, 0]) lid_assembled();
}

// --------------------------------------------------------------------------
// the knob: the panel enclosure's knob, on a stem that reaches the shaft
// --------------------------------------------------------------------------
// The knob sits 1 mm above the lid and its stem reaches down to take
// `knob_engage` of the shaft.
stem_len = max(0, lid_t + 1 + clear_front - enc_shaft_tip + knob_engage);

module stem_knob() {
    difference() {
        cylinder(d = stem_d, h = stem_len + 0.01);
        translate([0, 0, -0.1]) cylinder(d = 6.15, h = stem_len + 0.2);
        translate([0, 0, -0.1]) cylinder(d1 = 7.35, d2 = 6.15, h = 1.5);
    }
    translate([0, 0, stem_len]) knob();
}

// Printed cap-down, stem up: the stem is then a vertical tube and the knob
// over it is not an overhang. 16 is the panel knob's height.
module knob_print() translate([0, 0, stem_len + 16]) rotate([180, 0, 0]) stem_knob();

// --------------------------------------------------------------------------
// preview: what goes in the box
// --------------------------------------------------------------------------
module contents() {
    // the controller
    color("seagreen") translate([bx, by, bzf - pcb_w]) cube([pcb_t, pcb_h, pcb_w]);
    // the rail and what is on it
    color("silver")
        translate([rail_x, rail_y - rail[0] / 2, dev_z0]) cube([rail_len, rail[0], rail[1]]);
    for (i = [0 : len(devs) - 1])
        color(devs[i][3])
            translate([rail_x + rail_end + dev_x[i], rail_y - devs[i][1][1] / 2, dev_z0])
                cube(devs[i][1]);
    // -V1, base flush with the outside of the wall, and -E1 on it
    color("dimgray")
        translate([outer_x - ssr[2], ssr_yc - ssr[1] / 2, ssr_zc - ssr[0] / 2])
            cube([ssr[2], ssr[1], ssr[0]]);
    color("lightsteelblue")
        translate([outer_x, ssr_yc - hs[0] / 2, ssr_zc - hs[1] / 2]) {
            cube([6, hs[0], hs[1]]);
            for (z = [2 : 8 : hs[1] - 2])
                translate([6, 0, z]) cube([hs[2] - 6, hs[0], 2]);
        }
    // the display module behind the lid
    color("navy")
        translate([oled_xy[0] - oled_pcb[0] / 2, oled_xy[1] - oled_pcb[1] / 2,
                   top_in - oled_boss_h - 4])
            cube([oled_pcb[0], oled_pcb[1], 4]);
}

// --------------------------------------------------------------------------
// render
// --------------------------------------------------------------------------
if (part == "box") box();
else if (part == "lid") lid();
else if (part == "knob") knob_print();
else if (part == "open") { box(); contents(); }
else {
    box();
    contents();
    translate([0, 0, 60]) lid_assembled();
    translate([knob_xy[0], knob_xy[1], top_in + lid_t + 61 - stem_len]) stem_knob();
}

echo(str("OUTSIDE SIZE:  ", outer_x, " x ", outer_y, " x ", top_in + lid_t,
         " mm (w x h x d), ", outer_x + 2 * (ear_d + 4), " mm across the ears"));
echo(str("SELV BAY: ", selv_w, " mm   MAINS BAY: ", mains_w,
         " mm   RAIL: ", rail_len, " mm TS 35"));
echo(str("HEATSINK SCREWS from the SSR's centre (y, z): ",
         [for (s = hs_screws) [s[0] - ssr_yc, s[1] - ssr_zc]]));
echo(str("KNOB STEM: ", stem_len, " mm"));
