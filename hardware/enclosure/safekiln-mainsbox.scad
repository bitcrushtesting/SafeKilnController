// SPDX-FileCopyrightText: 2026 Bitcrush Testing
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wall-mount housing for the Safe Kiln Controller in two printed parts,
// stacked front to back: a BASE on the wall that carries everything at 230 V,
// and a TOP in front of it that carries the controller board behind its face.
//
//   openscad -D part=\"base\" -o mainsbase.stl safekiln-mainsbox.scad
//   openscad -D part=\"top\"  -o mainstop.stl  safekiln-mainsbox.scad
//   openscad -D part=\"all\"  -o preview.png   safekiln-mainsbox.scad
//
// Or `./slice.sh mainsbase mainstop knob`. The knob is the panel enclosure's.
//
// ===========================================================================
// WHAT GOES IN IT
// ===========================================================================
// For a single-phase kiln of up to 16 A at 230 V (3.6 kW):
//
//   -G1  RECOM RACM15E-12SK/PMAD    12 V 1.25 A, screwed to the base
//   -K1  Hongfa HF92F-012D-2C..     relay, 2 Form C, NO 30 A 250 VAC, 12 V
//                                   coil (1.7 W), QC tabs, flange mount
//   -V1  COMUS WG A5 6D 40 Z        SSR, zero-cross, 40 A, 3 to 32 V DC in
//   -E1  heatsink, at most 1.5 K/W  outside the base, fins vertical
//   -F1  5 x 20 mm fuse terminal    T 1 A, feeds -G1 only
//   -X1 to -X6                      L, N, PE in; PE, N, L out to the kiln
//
// -K1's two normally-open contacts break L and N together. A kiln on a
// Schuko plug can have the two swapped by turning the plug over, and a
// one-pole break would then open the neutral and leave the elements live.
//
// ===========================================================================
// HOW IT IS LAID OUT
// ===========================================================================
// X to the right, Y up the wall, Z out of the wall towards the operator.
//
//   BASE, z = 0 .. base_d, printed back-down. On its back plate: a short
//     DIN rail low down with -F1 and the terminals, the cable glands under it
//     in the floor; -G1 and -K1 above the rail, both standing upright and
//     screwed to bosses; -V1 in an aperture in the right wall with its base
//     plate on -E1 outside.
//
//   TOP, z = base_d .. base_d + top_d, printed face-down. The controller
//     board lies flat behind the face, component side forward, on four
//     standoffs off the face: the encoder shaft and the display come straight
//     through it. The display module is screwed to its own four bosses,
//     because the four header pins do not hold it. USB-C is reached through
//     a duct in the left wall; the thermocouple and lid-switch cables enter
//     through glands in the top's floor.
//
// THERE IS NO PRINTED BARRIER BETWEEN THE TWO. The board's back faces the
// mains devices across open air, so the separation between SELV and 230 V is
// clearance and wiring discipline: the clearances asserted below, and every
// mains conductor ferruled and tied so that one which comes loose cannot
// reach the board. That is a weaker argument than a wall, and README.md says
// so.
//
// ===========================================================================
// WHY THE SSR IS ON AN OUTSIDE HEATSINK
// ===========================================================================
// The WG A5 drops about 1.85 V peak, roughly 1.2 W per amp, so about 20 W at
// 16 A. Inside a closed plastic box that raises the air by tens of kelvin,
// and SWR-SAF-11 trips the controller on enclosure temperature. So its base
// plate goes through the wall onto a heatsink outside.
//
// ===========================================================================
// WHAT IS NOT YET KNOWN
// ===========================================================================
// Values marked CHECK reach the print and come from catalogues or from the
// footprint rather than from the parts in hand: the heatsink, the encoder's
// shaft and bushing heights, the display module and its header stack.
//
// PRINTED PLASTIC IS NOT A MAINS ENCLOSURE BY DEFAULT. A part that holds live
// terminals has to pass the glow-wire test of EN 60695-2-11 (or be UL 94 V-0);
// ordinary ASA does not. See README.md.

use <safekiln-enclosure.scad>   // rrect(), boss(), speaker_grid(), knob()

// --------------------------------------------------------------------------
// what to render
// --------------------------------------------------------------------------
part = "all";          // "base" | "top" | "knob" | "open" | "all"
$fn  = 48;

// --------------------------------------------------------------------------
// the board, read from hardware/safekiln.kicad_pcb
// --------------------------------------------------------------------------
// Board coordinates (u, v): u to the right, v DOWN, origin at the outline's
// top-left corner, as KiCad shows the front. The board is seen from the
// front in the housing, so u runs with X and v against Y.
pcb_w = 100.0;
pcb_h =  70.0;
pcb_t =   1.6;
pcb_hole_inset = 4.0;  // HWR-24's proposal: the board has no holes yet
h_below    = 3.5;      // THT leads behind the board
h_terminal = 11.0;     // MX126 terminal blocks, the tallest part but two

enc_uv  = [ 9.55, 51.15];   // SW3, EC11E vertical: the shaft stands off the board
usb_uv  = [ 4.10, 23.90];   // J2, opening out of the left edge
bz_uv   = [70.60, 17.45];   // BZ1
oled_hdr_u  = 33.45;        // J5, a vertical column of four sockets ...
oled_hdr_v  = 53.65;        // ... centred here

enc_shaft_h = 20.0;    // CHECK: shaft tip above the board (the footprint's H20)
enc_bush_h  = 11.5;    // CHECK: top of the threaded bushing above the board

// The display module. J5 is a column, so the module's header edge is
// vertical and the module lies to the right of it, turned 90 degrees from
// the panel enclosure's. The firmware has to draw rotated to match.
oled_pcb       = [27.0, 27.3];  // CHECK: along the header, away from it
oled_hdr_in    =  2.0;          // CHECK: header row in from the module's edge
oled_window    = [25.0, 14.5];  // CHECK: along the header, away from it
oled_window_off = 3.0;          // CHECK: window centre towards the header
oled_hole_grid = [23.0, 23.5];  // CHECK: 4 x M2
// How far the module's front stands off the board: socket, the header's
// plastic spacer, the module PCB, then the glass, which meets the face.
oled_socket_h = 8.5;   // CHECK
oled_spacer_h = 2.5;   // CHECK
oled_pcb_t    = 1.2;   // CHECK
oled_glass_t  = 1.6;   // CHECK
oled_boss_d   = 4.5;

// The knob, as in safekiln-enclosure.scad (its variables are not visible
// through `use`, only its modules).
knob_d = 26.0;
knob_h = 16.0;
knob_gap = 1.0;        // the knob sits in a hole in the face, this much clear

// --------------------------------------------------------------------------
// the devices in the base
// --------------------------------------------------------------------------
// -G1, RACM15E /PMAD, from the datasheet's drawing (Rev. 1-2025, page 8).
// It stands upright, AC end down, DIN latch under it.
psu       = [26.4, 83.0, 29.5];  // across, along, height off the back plate
psu_latch = 5.56;                // the DIN latch beyond the AC end
psu_fc    = [19.0, 75.0];        // 4 x 3.2 holes, fixing centres
psu_fc_off = [3.7, 4.0];         // first hole from the body's corner

// -K1, HF92F type 2 (QC), flange mount, from the datasheet's drawing.
relay        = [33.7, 68.6, 26.4];  // across, along incl. flange, body height
relay_slots  = 59.7;                // two 4.5 mm slots on the long centreline
relay_tabs   = 11.4;                // QC tabs above the body
relay_recept = 10.0;                // CHECK: a 6.3 mm receptacle and its sleeve

// -V1, WG A5 6D 40 Z. Its long axis runs along Z, so the aperture's printed
// bridge is its short side.
ssr = [57.2, 44.5, 23.0];   // along Z, along Y, into the base
hs  = [100.0, 66.0, 40.0];  // CHECK: along Y, along Z, fin depth
hs_screw_d = 4.5;           // M4 clearance; tap the heatsink M4

// The rail: [designation, [width along the rail, height, depth], gap, colour].
term  = [ 6.2, 47.5, 47.7];   // -X1..-X6 Phoenix Contact UT 4 / UT 4-PE
fuse  = [ 8.2, 75.0, 57.0];   // -F1 5 x 20 mm fuse terminal
endbr = [ 9.5, 46.0, 35.0];   // end bracket
devs = [
    ["end",    endbr, 0.0, "dimgray"],
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
rail_end    =  6.0;

// --------------------------------------------------------------------------
// the shell
// --------------------------------------------------------------------------
wall     = 2.4;
back_t   = 3.0;
front_t  = 3.0;
corner_r = 3.0;
lip_h    = 4.0;        // the top's skirt, reaching into the base
lip_t    = 1.6;
fit      = 0.3;

boss_d   = 8.0;
insert_d = 4.2;        // M3 heat-set
insert_h = 5.5;
insert4_d = 5.6;       // M4 heat-set, for -K1's flange
insert4_h = 6.0;

ear_t = 4.0;
ear_d = 12.0;
ear_hole_d = 5.0;

margin     = 8.0;      // inner wall to the nearest device
wire_below = 30.0;     // under the rail: the glands and the field cables
dev_gap    = 12.0;     // between devices: room for a ferrule and a bend
base_clear = 4.0;      // in front of the deepest device, to the base's rim

// The top's interior around the board.
clear_left  = 18.0;    // J1, J10 and the USB socket leave this edge
clear_right = 30.0;    // five terminal blocks leave this one
clear_above = 22.0;
behind_board = 8.0;    // behind the board's leads, before the top's rim

usb_duct = [13.0, 7.5];  // along the board's edge, across: a USB-C overmould

// --------------------------------------------------------------------------
// derived: the base
// --------------------------------------------------------------------------
function sum(v, n) = n <= 0 ? 0 : sum(v, n - 1) + v[n - 1];
dev_pitch = [for (d = devs) d[2] + d[1][0]];
dev_x     = [for (i = [0 : len(devs) - 1]) sum(dev_pitch, i) + devs[i][2]];
rail_len  = sum(dev_pitch, len(devs)) + 2 * rail_end;
rail_tall = max([for (d = devs) d[1][1]]);
dev_deep  = max([for (d = devs) d[1][2]]);

// Inner coordinates of the base, from its inner bottom-left corner.
rail_x  = margin;
rail_yc = wire_below + rail_tall / 2;
upper_y = wire_below + rail_tall + dev_gap;       // where -G1 and -K1 start

psu_x   = margin;
psu_y   = upper_y + psu_latch;                    // the body, above its latch
relay_x = psu_x + psu[0] + dev_gap;
relay_y = psu_y + (psu[1] - relay[1]) / 2;

relay_depth = relay[2] + relay_tabs + relay_recept;
base_in_d = max(rail_boss_h + rail[1] + dev_deep,
                relay_depth, psu[2], ssr[0] + 6) + base_clear;
base_d    = back_t + base_in_d;                   // the base's rim

// --------------------------------------------------------------------------
// derived: the top, and the common outline
// --------------------------------------------------------------------------
// The board's position, inner coordinates: its left edge and its top edge.
in_w = max(clear_left + pcb_w + clear_right,
           relay_x + relay[0] + dev_gap + ssr[2] + 20);
in_h = max(psu_y + psu[1] + margin, clear_above + pcb_h + 60);

outer_x = in_w + 2 * wall;
outer_y = in_h + 2 * wall;
right_x = wall + in_w;                            // right wall, inner face

board_g  = oled_socket_h + oled_spacer_h + oled_pcb_t + oled_glass_t;
top_in_d = board_g + pcb_t + h_below + behind_board;
top_d    = top_in_d + front_t;
front_z  = base_d + top_d;                        // the operator's face
face_in  = front_z - front_t;                     // its inner side
board_zf = face_in - board_g;                     // the board's component side

bx0 = wall + clear_left;                          // board u = 0
by0 = wall + in_h - clear_above;                  // board v = 0
function uv(p) = [bx0 + p[0], by0 - p[1]];

knob_xy = uv(enc_uv);
bz_xy   = uv(bz_uv);
usb_xy  = uv(usb_uv);
oled_c  = uv([oled_hdr_u - oled_hdr_in + oled_pcb[1] / 2, oled_hdr_v]);
// in the housing the module's "along the header" runs along Y
oled_box = [oled_pcb[1], oled_pcb[0]];
oled_win_c = oled_c - [oled_window_off, 0];      // towards the header, at left
oled_win   = [oled_window[1], oled_window[0]];
oled_holes = [for (sx = [-1, 1], sy = [-1, 1])
                  oled_c + [sx * oled_hole_grid[1] / 2, sy * oled_hole_grid[0] / 2]];

board_holes = [for (u = [pcb_hole_inset, pcb_w - pcb_hole_inset],
                    v = [pcb_hole_inset, pcb_h - pcb_hole_inset]) uv([u, v])];

knob_bottom = enc_bush_h + 0.5;                   // above the board
knob_engage = enc_shaft_h - knob_bottom;

// -V1 and -E1, on the right wall, beside -K1 rather than above anything.
ssr_yc = wall + relay_y + relay[1] / 2;
ssr_zc = back_t + base_in_d / 2;
hs_screws = [for (sy = [-1, 1], sz = [-1, 1])
                 [ssr_yc + sy * (ssr[1] / 2 + 7.0), ssr_zc + sz * 15.0]];

// Corner bosses: inserts in the base's rim, the top screws into them.
bi = boss_d / 2;
boss_pos = [
    [wall + bi,    wall + bi],
    [wall + bi,    outer_y - wall - bi],
    [right_x - bi, wall + bi],
    [right_x - bi, outer_y - wall - bi],
];
screw_reach = 22.0;    // the top's column under the screw head: M3 x 25

// Glands: [x from the inner left, z, bore]. Mains in the base's floor, the
// thermocouples and the lid switch in the top's.
glands_base = [[22, 0, 25.3], [56, 0, 25.3]];                    // supply, kiln
glands_top  = [[bx0 + 20, 0, 16.3], [bx0 + 50, 0, 16.3], [bx0 + 80, 0, 16.3]];

ears = [[0, 20], [0, outer_y - 20],
        [outer_x, 20], [outer_x, ssr_yc - hs[0] / 2 - ear_d]];

// --------------------------------------------------------------------------
// checks
// --------------------------------------------------------------------------
assert(relay_x + relay[0] + dev_gap <= in_w - ssr[2] - 10,
       "-K1 crowds -V1's terminals");
assert(rail_x + rail_len <= in_w - margin, "the rail does not fit");
assert(ssr_yc + hs[0] / 2 <= outer_y, "the heatsink stands above the housing");
assert(hs[1] <= base_d, "the heatsink is deeper than the base");
assert(ssr_zc + ssr[0] / 2 < base_d - 1, "-V1 reaches past the base's rim");
assert(max([for (s = hs_screws) s[1]]) + hs_screw_d / 2 < base_d - 2,
       "a heatsink screw breaks out of the rim");
// Nothing on the board's component side may reach the face but the display.
assert(h_terminal < board_g - 1.0, "the terminal blocks touch the face");
assert(enc_bush_h < board_g, "the encoder's bushing touches the face");
// The board and its leads stay in the top, clear of the base's devices.
assert(board_zf - pcb_t - h_below >= base_d + 2,
       "the board's leads reach into the base");
assert(knob_engage >= 6.0, "the knob takes too little of the shaft");
assert(bx0 >= wall + clear_left - 0.01 && bx0 + pcb_w <= right_x - clear_right + 0.01,
       "the board does not fit across the top");
assert(knob_xy[0] - knob_d / 2 - knob_gap > wall + 2, "the knob hits the left wall");

// --------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------
// A horizontal hole that prints without support: a teardrop pointing up the
// print. The base prints back-down, so up is +Z; the top prints face-down,
// so up is -Z, and its holes use the mirrored one.
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
module hole_y_down(d, l) mirror([0, 0, 1]) hole_y(d, l);

// --------------------------------------------------------------------------
// the base
// --------------------------------------------------------------------------
module base() {
    difference() {
        union() {
            difference() {
                rrect([outer_x, outer_y], corner_r, base_d);
                translate([wall, wall, back_t])
                    rrect([in_w, in_h], max(corner_r - wall, 0.5), base_in_d + 1);
            }

            for (p = boss_pos)
                translate([p[0], p[1], back_t]) cylinder(d = boss_d, h = base_in_d);

            // the rail
            for (x = [rail_x + 4.5, rail_x + rail_len - 4.5])
                translate([wall + x, wall + rail_yc, back_t])
                    cylinder(d = 9.0, h = rail_boss_h);

            // -G1's four bosses
            for (h = psu_holes())
                translate([h[0], h[1], back_t]) cylinder(d = 8.0, h = 5.0);

            // -K1's two
            for (h = relay_holes())
                translate([h[0], h[1], back_t]) cylinder(d = 10.0, h = 5.0);

            for (e = ears)
                hull() {
                    translate([e[0] + (e[0] == 0 ? -1 : 1) * (ear_d / 2 + 2), e[1], 0])
                        cylinder(d = ear_d, h = ear_t);
                    translate([e[0] - 1, e[1] - ear_d / 2 - 2, 0])
                        cube([2, ear_d + 4, ear_t]);
                }
        }

        // the top's inserts
        for (p = boss_pos)
            translate([p[0], p[1], base_d - insert_h])
                cylinder(d = insert_d, h = insert_h + 1);

        // blind inserts: nothing goes through to the wall
        for (x = [rail_x + 4.5, rail_x + rail_len - 4.5])
            translate([wall + x, wall + rail_yc, back_t + rail_boss_h - insert_h + 1])
                cylinder(d = insert_d, h = insert_h);
        for (h = psu_holes())
            translate([h[0], h[1], back_t + 5.0 - insert_h + 1])
                cylinder(d = insert_d, h = insert_h);
        for (h = relay_holes())
            translate([h[0], h[1], back_t + 5.0 - insert4_h + 1])
                cylinder(d = insert4_d, h = insert4_h);

        for (e = ears)
            translate([e[0] + (e[0] == 0 ? -1 : 1) * (ear_d / 2 + 2), e[1], -1])
                cylinder(d = ear_hole_d, h = ear_t + 2);

        // the mains glands, in the floor
        for (g = glands_base)
            translate([wall + g[0], wall / 2, back_t + base_in_d / 2])
                hole_y(g[2], wall + 2);

        // -V1 through the right wall, -E1's screws around it
        translate([right_x - 1, ssr_yc - ssr[1] / 2 - 0.5, ssr_zc - ssr[0] / 2 - 0.5])
            cube([wall + 2, ssr[1] + 1.0, ssr[0] + 1.0]);
        for (s = hs_screws)
            translate([right_x + wall / 2, s[0], s[1]]) hole_x(hs_screw_d, wall + 2);

        // ventilation: 2 mm slots (IP3X), low in the floor and high in the
        // left wall, none in anything that faces up
        for (x = [wall + 80 : 6 : right_x - 12])
            translate([x - 1, -1, back_t + 8]) cube([2, wall + 2, base_in_d - 20]);
        for (y = [outer_y - 40 : 5 : outer_y - 14])
            translate([-1, y - 1, back_t + 8]) cube([wall + 2, 2, base_in_d - 20]);
    }
}

function psu_holes() = [for (a = [0, 1], c = [0, 1])
    [wall + psu_x + psu_fc_off[0] + a * psu_fc[0],
     wall + psu_y + psu_fc_off[1] + c * psu_fc[1]]];
function relay_holes() = [for (s = [-1, 1])
    [wall + relay_x + relay[0] / 2, wall + relay_y + relay[1] / 2 + s * relay_slots / 2]];

// --------------------------------------------------------------------------
// the top
// --------------------------------------------------------------------------
// Modelled where it sits on the base, turned over for printing.
module top_assembled() {
    sk = [in_w - 2 * fit, in_h - 2 * fit];
    difference() {
        union() {
            // walls and face
            translate([0, 0, base_d])
                difference() {
                    rrect([outer_x, outer_y], corner_r, top_d);
                    translate([wall, wall, -1])
                        rrect([in_w, in_h], max(corner_r - wall, 0.5), top_in_d + 1);
                }

            // the skirt into the base
            translate([wall + fit, wall + fit, base_d - lip_h])
                difference() {
                    rrect(sk, 0.5, lip_h + 0.01);
                    translate([lip_t, lip_t, -0.1])
                        rrect(sk - [2 * lip_t, 2 * lip_t], 0.5, lip_h + 0.3);
                }

            // the corner columns the screws pass through
            for (p = boss_pos)
                translate([p[0], p[1], base_d]) cylinder(d = boss_d, h = top_d);

            // the board's standoffs, off the face
            for (h = board_holes)
                translate([h[0], h[1], board_zf]) cylinder(d = 7.0, h = board_g + 0.01);

            // the display's bosses: the glass meets the face, the module's
            // PCB sits on these, and four M2 screws hold it
            for (h = oled_holes)
                translate([h[0], h[1], face_in - oled_glass_t])
                    cylinder(d = oled_boss_d, h = oled_glass_t + 0.01);

            // the USB duct's walls, from the left wall to the socket
            translate([wall - 0.01, usb_xy[1] - usb_duct[0] / 2 - 1.6,
                       board_zf + 1.6 - usb_duct[1] / 2 - 1.6])
                cube([bx0 - wall - 1.0, usb_duct[0] + 3.2, usb_duct[1] + 3.2]);
        }

        // the skirt clears the bosses
        for (p = boss_pos)
            translate([p[0], p[1], base_d - lip_h - 0.1])
                cylinder(d = boss_d + 2 * fit, h = lip_h + 0.1);

        // M3 x 25 through the columns, heads sunk into the face
        for (p = boss_pos)
            translate([p[0], p[1], base_d - 1]) {
                cylinder(d = 3.4, h = top_d + 2);
                translate([0, 0, screw_reach + 1]) cylinder(d = 6.5, h = top_d);
            }

        // pilot holes: 2.6 taps M3 for the board, 1.7 taps M2 for the display
        for (h = board_holes)
            translate([h[0], h[1], board_zf - 0.1]) cylinder(d = 2.6, h = 8);
        for (h = oled_holes)
            translate([h[0], h[1], face_in - oled_glass_t - 0.1]) cylinder(d = 1.7, h = 5);

        // the display window, chamfered on the outside
        translate([oled_win_c[0], oled_win_c[1], face_in - 0.1]) {
            linear_extrude(front_t + 0.2) square(oled_win, center = true);
            translate([0, 0, front_t * 0.45])
                linear_extrude(front_t * 0.55 + 0.2)
                    square(oled_win + [2.0, 2.0], center = true);
        }

        // the knob drops into the face down to the bushing
        translate([knob_xy[0], knob_xy[1], face_in - 1])
            cylinder(d = knob_d + 2 * knob_gap, h = front_t + 2);

        // the USB duct's bore, out through the left wall
        translate([-1, usb_xy[1] - usb_duct[0] / 2, board_zf + 1.6 - usb_duct[1] / 2])
            cube([bx0 + 0.5, usb_duct[0], usb_duct[1]]);

        // the buzzer
        translate([bz_xy[0], bz_xy[1], front_z - front_t / 2]) speaker_grid();

        // the field glands for the sensors, in the top's floor
        for (g = glands_top)
            translate([g[0], wall / 2, base_d + top_in_d / 2])
                hole_y_down(g[2], wall + 2);

        // ventilation, high in both side walls
        for (y = [outer_y - 34 : 5 : outer_y - 12], x = [-1, right_x - 1])
            translate([x, y - 1, base_d + 6]) cube([wall + 2, 2, top_in_d - 12]);

        // the warning, engraved into the face below the board
        translate([outer_x / 2, (wall + by0 - pcb_h) / 2, front_z - 0.6])
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

module top() translate([0, outer_y, front_z]) rotate([180, 0, 0]) top_assembled();

// --------------------------------------------------------------------------
// preview: what goes in it
// --------------------------------------------------------------------------
module contents() {
    // the rail and its devices
    color("silver")
        translate([wall + rail_x, wall + rail_yc - rail[0] / 2, back_t + rail_boss_h])
            cube([rail_len, rail[0], rail[1]]);
    for (i = [0 : len(devs) - 1])
        color(devs[i][3])
            translate([wall + rail_x + rail_end + dev_x[i],
                       wall + rail_yc - devs[i][1][1] / 2, back_t + rail_boss_h])
                cube(devs[i][1]);
    // -G1, on its bosses, latch below
    color("white") translate([wall + psu_x, wall + psu_y, back_t + 5]) cube(psu);
    color("gray")
        translate([wall + psu_x + 4.45, wall + psu_y - psu_latch, back_t + 5])
            cube([17.5, psu_latch, 3]);
    // -K1, body and tabs
    color("orange")
        translate([wall + relay_x, wall + relay_y + (relay[1] - 52) / 2, back_t + 5])
            cube([relay[0], 52, relay[2]]);
    color("goldenrod")
        translate([wall + relay_x + 6, wall + relay_y + 14, back_t + 5 + relay[2]])
            cube([relay[0] - 12, 40, relay_tabs]);
    // -V1 and -E1
    color("dimgray")
        translate([outer_x - ssr[2], ssr_yc - ssr[1] / 2, ssr_zc - ssr[0] / 2])
            cube([ssr[2], ssr[1], ssr[0]]);
    color("lightsteelblue")
        translate([outer_x, ssr_yc - hs[0] / 2, ssr_zc - hs[1] / 2]) {
            cube([6, hs[0], hs[1]]);
            for (z = [2 : 8 : hs[1] - 2])
                translate([6, 0, z]) cube([hs[2] - 6, hs[0], 2]);
        }
    // the board, the encoder and the display
    color("seagreen") translate([bx0, by0 - pcb_h, board_zf - pcb_t]) cube([pcb_w, pcb_h, pcb_t]);
    color("silver") translate([knob_xy[0], knob_xy[1], board_zf]) cylinder(d = 6, h = enc_shaft_h);
    color("navy")
        translate([oled_c[0] - oled_box[0] / 2, oled_c[1] - oled_box[1] / 2,
                   face_in - oled_glass_t - oled_pcb_t])
            cube([oled_box[0], oled_box[1], oled_pcb_t + oled_glass_t]);
}

// --------------------------------------------------------------------------
// render
// --------------------------------------------------------------------------
if (part == "base") base();
else if (part == "top") top();
else if (part == "knob") knob();
else if (part == "open") { base(); contents(); }
else {
    base();
    contents();
    translate([0, 0, 50]) top_assembled();
    translate([knob_xy[0], knob_xy[1], board_zf + knob_bottom + 50]) knob();
}

echo(str("OUTSIDE SIZE:  ", outer_x, " x ", outer_y, " x ", front_z,
         " mm (w x h x d), ", outer_x + 2 * (ear_d + 4), " mm across the ears"));
echo(str("BASE: ", base_d, " mm deep   TOP: ", top_d, " mm deep   RAIL: ",
         rail_len, " mm TS 35"));
echo(str("BOARD: component side ", board_g, " mm behind the face"));
echo(str("KNOB: takes ", knob_engage, " mm of the shaft, stands ",
         board_zf + knob_bottom + knob_h - front_z, " mm proud of the face"));
echo(str("HEATSINK SCREWS from the SSR's centre (y, z): ",
         [for (s = hs_screws) [s[0] - ssr_yc, s[1] - ssr_zc]]));
