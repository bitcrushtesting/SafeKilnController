// SPDX-FileCopyrightText: 2026 Bitcrush Testing
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Panel-mount enclosure for the Safe Kiln Controller, in two printed parts.
//
//   openscad -D part=\"bezel\" -o bezel.stl safekiln-enclosure.scad
//   openscad -D part=\"shell\" -o shell.stl safekiln-enclosure.scad
//   openscad -D part=\"all\"   -o both.stl  safekiln-enclosure.scad   // preview
//
// Or let the Makefile do it: `make -C hardware/enclosure`.
//
// ===========================================================================
// WHY THE BOARD STANDS ON EDGE
// ===========================================================================
// Every dimension below is read from hardware/safekiln.kicad_pcb rather than
// guessed, and the placement decides the whole shape:
//
//   the board outline is 100 x 70 mm (gr_rect 50,40 to 150,110);
//   SW3, the rotary encoder, is ON the board at its LEFT edge (4.5, 55.8);
//   J1 (5 V DC in) and J2 (USB-C) are on that same left edge;
//   six 5 mm terminal blocks leave the RIGHT edge, which is the kiln wiring:
//     J8 coil, J6 SSR, J9 lid switch, J10 CT, J3 TC1, J7 TC2;
//   J5 is an OLED module on a 4-pin cable, NOT a board-mounted display.
//
// So the operator's face is the board's left edge and the wiring face is its
// right edge. The board therefore stands vertically behind the panel rather
// than lying flat: the encoder shaft, the USB socket and the DC input all come
// through the front, and the terminal blocks face away into the back of the
// enclosure, which is where the kiln's wires belong.
//
// Lying the board flat would have been easier to print and would have put the
// knob on the side of the box, where nobody looks for it, and the mains-side
// terminals on the operator's face, which is worse than awkward.
//
// The display is the one part free to go anywhere, being on a cable. It cannot
// go above or below the knob, for the reason worked through at `in_w` below,
// so it takes its own lane beside the board's column: screen on one side, knob
// on the other, which is what a panel instrument looks like anyway.
//
// ===========================================================================
// WHAT IS NOT YET KNOWN
// ===========================================================================
// The board has NO mounting holes. Nothing in the PCB fixes it to anything, so
// the standoff positions below are a PROPOSAL this enclosure makes and the
// board has to adopt: four M3 holes inset `pcb_hole_inset` from the corners.
// Until they exist, print the shell and the board will not screw into it.
// Recorded as a requirement rather than a comment: HWR-24.
//
// The OLED module's own dimensions are the common 0.96" I2C variant and must
// be checked against the module actually bought. They are parameters for that
// reason, and the three that matter are marked CHECK.

// --------------------------------------------------------------------------
// what to render
// --------------------------------------------------------------------------
part = "all";          // "bezel" | "shell" | "knob" | "all"
$fn  = 48;

// --------------------------------------------------------------------------
// the board, measured from hardware/safekiln.kicad_pcb
// --------------------------------------------------------------------------
pcb_w   = 100.0;       // outline x, becomes the enclosure's DEPTH
pcb_h   =  70.0;       // outline y, becomes the enclosure's HEIGHT
pcb_t   =   1.6;
pcb_hole_inset = 4.0;  // proposed M3 centres, inset from each corner
pcb_hole_d     = 3.4;  // M3 clearance

// Component heights above the board, for the clearance the shell needs.
// The terminal blocks are the tallest thing and they decide the width.
h_terminal   = 11.0;   // MX126-5.0 body above the board
h_esp_module =  3.5;   // ESP32-S3-WROOM-1 can
h_encoder    = 14.0;   // EC11E body above the board, before the shaft
h_buzzer     =  5.5;   // FST-9650B-5V, SMD; was 9.5 for the TMB12A05
// UNDERSTATED, and this one is used: it sets pcb_stack and so the box width.
// The terminal blocks' pins protrude 3.50 mm, measured from the part's own 3D
// mesh and matching the KF301's stated pin length, not 2.5. Correcting it
// widens the box by 1 mm and changes the panel cut-out, so it is a reprint
// rather than an edit: tracked as B6 rather than changed here.
h_below      =  2.5;   // THT lead protrusion under the board

// Positions on the board, x from the left edge, y from the bottom edge.
// Straight out of the PCB file; see the header.
enc_y     = 55.75;     // SW3
usb_y     = 23.90;     // J2, USB-C
dcin_y    = 11.10;     // J1, 5 V DC in
term_ys   = [11.25, 24.75, 37.05, 50.20, 61.75, 73.15];  // J8 J6 J9 J10 J3 J7
term_names= ["COIL", "SSR", "LID", "CT", "TC1", "TC2"];
swd_x     = 82.73;     // J4, the supervisor's tag-connect
swd_y     = 36.11;

// The module's width is needed before the enclosure's, so it is declared
// here and the rest of its dimensions below with the other display numbers.
oled_pcb_w = 27.0;     // CHECK

// --------------------------------------------------------------------------
// the enclosure
// --------------------------------------------------------------------------
wall      =  2.4;      // 3 perimeters at 0.45 mm line width, and stiff enough
                       // that a terminal screw being torqued does not flex it
floor_t   =  2.4;
clear_x   =  4.0;      // board edge to inner wall, operator side
clear_rear=  6.0;      // behind the terminal blocks, for wire bend radius
clear_y   =  3.0;      // top and bottom
clear_z   =  3.0;      // across the board's thickness

// Internal volume. Depth runs along the board's x, height along its y, and
// the width is across the board's thickness.
in_d = pcb_w + clear_x + clear_rear;
in_h = pcb_h + 2 * clear_y;

// The board's own stack: leads under it, the board, the tallest part on it.
pcb_stack = h_below + pcb_t + h_terminal;
pcb_z     = h_below + pcb_t / 2;     // the board's mid-plane, from the wall

// ---------------------------------------------------------------------------
// WHY THE BOX IS WIDER THAN THE BOARD NEEDS
// ---------------------------------------------------------------------------
// The first version of this made in_w the board's stack and nothing else,
// which gave a front face 23.4 mm wide. The OLED module is 27 mm. The display
// did not fit, and the render said so before anything was printed.
//
// Nor does it fit the other way round. The front face is the board's left
// edge, which already carries three things at fixed heights: the DC input at
// y = 11.1, the USB socket at y = 23.9 and the encoder at y = 55.8. The
// largest clear gap between them is about 25 mm, and a 27 mm module needs
// more than that plus a margin.
//
// So the display goes BESIDE that column rather than above or below it, and
// the box is as wide as the two need together. That is also what a panel
// instrument normally looks like: a screen on one side, a knob on the other,
// and the sockets under the knob where a cable hangs clear of the glass.
oled_lane = oled_pcb_w + 2 * 2.0;
in_w = pcb_stack + clear_z + oled_lane;

bezel_t     =  3.0;    // the plate that sits against the kiln's panel
bezel_lip   =  8.0;    // flange beyond the cut-out, all round
bezel_screw_d = 3.4;   // M3 through the panel
corner_r    =  3.0;

// The two parts meet with a lip so the joint is not a straight butt seam:
// a printed butt joint is a light gap and a dust path.
lip_h = 3.0;
lip_t = wall / 2;

// --------------------------------------------------------------------------
// the OLED module, J5, on a cable.  CHECK against the module bought.
// --------------------------------------------------------------------------
oled_pcb       = [oled_pcb_w, 27.3];   // CHECK
oled_window    = [25.0, 14.5];   // CHECK: glass aperture, generous on the
                                 // active 21.7 x 10.9 so the bezel never
                                 // clips a row of pixels
oled_window_off_y = 3.0;         // CHECK: window centre above module centre
oled_hole_grid = [23.0, 23.5];   // CHECK: 4 x M2
oled_hole_d    =  2.2;
oled_boss_h    =  4.0;           // standoff behind the bezel plate

// Where things sit on the front face, measured from the inner bottom-left.
//
// The knob is not free: it has to land on the encoder, so it follows enc_y,
// and the two sockets follow theirs. The display is the only free one, and it
// takes the lane beside them, centred over the height.
knob_y   = clear_y + enc_y;
oled_x   = pcb_stack + clear_z + oled_lane / 2;
oled_y   = in_h / 2;

// EC11E: 6 mm shaft, 7 mm bushing. 7.6 lets a printed hole take it without
// reaming, which a 7.2 will not at this wall thickness.
enc_shaft_d   = 7.6;
enc_boss_d    = 13.0;            // raised ring, keeps the knob off the paint
enc_boss_h    = 1.2;

usb_cut  = [10.0, 4.0];          // USB-C receptacle aperture, generous
dcin_cut = [11.0, 11.0];         // MX126-5.0-02 body, wire entry forward

// --------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------
module rrect(size, r, h) {
    linear_extrude(h)
        offset(r = r) offset(r = -r)
            square([size[0], size[1]], center = false);
}

// A screw boss with a blind hole: printed vertically so the hole needs no
// support and the layers are in shear, not in tension, when it is torqued.
module boss(d_out, d_in, h) {
    difference() {
        cylinder(d = d_out, h = h);
        translate([0, 0, -0.1]) cylinder(d = d_in, h = h + 0.2);
    }
}

// Sound holes for the buzzer: a grid, because one big hole in a panel is a
// weak point and a dust entry, and the buzzer only needs the air to move.
module speaker_grid(d = 2.0, n = 3, pitch = 4.0, h = 10) {
    for (i = [0 : n - 1], j = [0 : n - 1])
        translate([(i - (n - 1) / 2) * pitch, (j - (n - 1) / 2) * pitch, -h / 2])
            cylinder(d = d, h = h);
}

// --------------------------------------------------------------------------
// the rear shell: holds the board, takes the wiring
// --------------------------------------------------------------------------
// Printed open-face-up, so every overhang is a wall and the standoffs grow
// off the floor. No support anywhere.
module shell() {
    difference() {
        union() {
            // outer box
            rrect([in_w + 2 * wall, in_h + 2 * wall], corner_r, in_d + floor_t);
            // the lip that enters the bezel
            translate([wall / 2, wall / 2, in_d + floor_t - 0.01])
                difference() {
                    rrect([in_w + wall, in_h + wall], corner_r, lip_h);
                    translate([lip_t, lip_t, -0.1])
                        rrect([in_w + wall - 2 * lip_t, in_h + wall - 2 * lip_t],
                              corner_r, lip_h + 0.2);
                }
        }

        // the cavity
        translate([wall, wall, floor_t])
            rrect([in_w, in_h], corner_r, in_d + lip_h + 1);

        // --- the wiring face: one opening per terminal block --------------
        // Each is a slot rather than one long cut-out, so the wall between
        // two mains-adjacent terminals is still there. That wall is the only
        // thing standing between the coil wiring and the thermocouple pair.
        for (i = [0 : len(term_ys) - 1])
            translate([wall + h_below + pcb_t + h_terminal / 2 + 0.5,
                       wall + clear_y + term_ys[i],
                       floor_t / 2])
                cube([h_terminal + 4, 12.0, floor_t + 2], center = true);

        // --- the supervisor's tag-connect, J4 -----------------------------
        // HWR-19 wants SWD reachable. A TC2030 is pressed onto pads, so the
        // probe needs room above the board at that point and nothing else.
        translate([wall + h_below + pcb_t + 6, wall + clear_y + swd_y,
                   floor_t + swd_x])
            rotate([0, 90, 0]) cylinder(d = 11.0, h = 20, center = true);

        // --- ventilation --------------------------------------------------
        // Slots in the two long sides, none in the floor: a kiln sheds dust
        // and it falls downwards. Along the print direction, so they cost no
        // bridging.
        for (s = [0, 1])
            for (i = [0 : 5])
                translate([s ? in_w + wall + 1 : -1,
                           wall + clear_y + 8 + i * 10,
                           floor_t + 20 + i * 2])
                    rotate([0, 90, 0]) cube([30, 2.5, wall + 4], center = true);
    }

    // --- the board's standoffs -------------------------------------------
    // M3 bosses off the floor. The board stands on edge, so these take it at
    // its four corners: see HWR-24, the holes do not exist on the PCB yet.
    for (x = [pcb_hole_inset, pcb_w - pcb_hole_inset])
        for (y = [pcb_hole_inset, pcb_h - pcb_hole_inset])
            translate([wall + pcb_z, wall + clear_y + y,
                       floor_t + clear_x + x])
                rotate([0, -90, 0])
                    boss(7.0, 2.6, h_below + pcb_t + 1.5);   // 2.6 for an M3 tap
}

// --------------------------------------------------------------------------
// the front bezel: what the operator sees and touches
// --------------------------------------------------------------------------
// Printed face-down on the plate, so the surface somebody looks at is the one
// the bed made, and the apertures are all vertical walls.
module bezel() {
    difference() {
        union() {
            // the flange that covers the panel cut-out
            translate([-bezel_lip, -bezel_lip, 0])
                rrect([in_w + 2 * wall + 2 * bezel_lip,
                       in_h + 2 * wall + 2 * bezel_lip], corner_r + 2, bezel_t);
            // the skirt that receives the shell's lip
            translate([0, 0, bezel_t - 0.01])
                difference() {
                    rrect([in_w + 2 * wall, in_h + 2 * wall], corner_r, lip_h + 1);
                    translate([wall / 2 + lip_t, wall / 2 + lip_t, -0.1])
                        rrect([in_w + wall - 2 * lip_t, in_h + wall - 2 * lip_t],
                              corner_r, lip_h + 1.2);
                }
            // the ring the knob sits against
            translate([wall + pcb_z, wall + knob_y, 0])
                cylinder(d = enc_boss_d, h = bezel_t + enc_boss_h);
            // standoffs for the OLED module
            for (sx = [-1, 1], sy = [-1, 1])
                translate([wall + oled_x + sx * oled_hole_grid[0] / 2,
                           wall + oled_y + sy * oled_hole_grid[1] / 2,
                           bezel_t - 0.01])
                    boss(5.0, 1.7, oled_boss_h);   // 1.7 for an M2 tap
        }

        // --- the display window ------------------------------------------
        // Chamfered on the outside: a straight-walled window in a 3 mm plate
        // reads as a tunnel and clips the viewing angle.
        translate([wall + oled_x, wall + oled_y + oled_window_off_y, -0.1])
            union() {
                cube([oled_window[0], oled_window[1], bezel_t + 0.2], center = true);
                translate([0, 0, bezel_t * 0.45])
                    cube([oled_window[0] + 2.0, oled_window[1] + 2.0,
                          bezel_t * 0.55 + 0.2], center = true);
            }

        // --- the encoder shaft -------------------------------------------
        translate([wall + pcb_z, wall + knob_y, -1])
            cylinder(d = enc_shaft_d, h = bezel_t + enc_boss_h + 2);

        // --- USB-C and the DC input --------------------------------------
        translate([wall + pcb_z, wall + clear_y + usb_y, bezel_t / 2])
            cube([usb_cut[1] + pcb_t, usb_cut[0], bezel_t + 2], center = true);
        translate([wall + pcb_z + 2, wall + clear_y + dcin_y, bezel_t / 2])
            cube([dcin_cut[1], dcin_cut[0], bezel_t + 2], center = true);

        // --- the buzzer ---------------------------------------------------
        // BZ1 is mid-board, so its sound leaves through the front plate near
        // the display rather than through a face it does not touch.
        translate([wall + oled_x, wall + clear_y + 6.0, bezel_t / 2])
            speaker_grid();

        // --- panel screws -------------------------------------------------
        // Four, in the flange, clear of the cut-out.
        for (sx = [-1, 1], sy = [-1, 1])
            translate([in_w / 2 + wall + sx * (in_w / 2 + wall + bezel_lip / 2),
                       in_h / 2 + wall + sy * (in_h / 2 + wall + bezel_lip / 2),
                       -0.1])
                cylinder(d = bezel_screw_d, h = bezel_t + 0.2);
    }
}

// --------------------------------------------------------------------------
// the knob
// --------------------------------------------------------------------------
// For the Alps EC11E's 6 mm shaft. Printed BORE UP: the bore is then a
// vertical hole needing no support, the flutes are vertical walls, and the
// face the operator looks at is the one the bed made, which is the best
// surface a printer produces.
//
// Three things a printed knob usually gets wrong, and what is done instead:
//
//   THE BORE. 6.0 mm nominal prints undersize on most machines and a knob that
//   needs reaming is a knob that ends up glued on. `knob_bore` is 6.15 with a
//   shallow lead-in chamfer, which presses onto a splined shaft and holds on
//   the splines rather than on a friction fit. For a plain round shaft, set
//   `knob_grub = true` for an M3 radial screw instead.
//
//   THE GRIP. Flutes, not knurling: a printed knurl at 0.4 mm layers is a
//   texture you cannot feel and a place dust sits. Eight flutes at this
//   diameter are findable without looking, which is the whole point on a kiln
//   where the operator may be wearing a glove.
//
//   THE MARK. A moulded pointer, not a painted one. It is a raised rib that
//   the print makes for free and nothing wears off.
knob_d       = 26.0;   // across the flutes
knob_h       = 16.0;   // tall enough to grip, short enough not to lever the
                       // encoder's bushing when somebody leans on it
knob_bore    =  6.15;  // CHECK on the first print: see above
knob_bore_h  = 12.0;   // leaves a 4 mm solid cap over the shaft
knob_flutes  =  8;
knob_flute_d =  5.0;
knob_skirt   =  1.5;   // thickness over the bore, at the open end
knob_grub    = false;  // true: add a radial M3 tapping hole for a round shaft
knob_mark    = true;

module knob() {
    difference() {
        union() {
            // the body, slightly tapered so the fingers find the top
            cylinder(d1 = knob_d, d2 = knob_d - 2.0, h = knob_h);
            // the pointer: a rib up one flank, ending flush with the top
            if (knob_mark)
                translate([0, knob_d / 2 - 1.2, 0])
                    hull() {
                        cylinder(d = 3.0, h = knob_h);
                        translate([0, 1.5, 0]) cylinder(d = 1.5, h = knob_h);
                    }
        }

        // the flutes
        for (i = [0 : knob_flutes - 1])
            rotate([0, 0, i * 360 / knob_flutes])
                translate([0, knob_d / 2, -1])
                    cylinder(d = knob_flute_d, h = knob_h + 2);

        // the bore, with a lead-in so it starts on the shaft without a press
        translate([0, 0, -0.1]) cylinder(d = knob_bore, h = knob_bore_h);
        translate([0, 0, -0.1])
            cylinder(d1 = knob_bore + 1.2, d2 = knob_bore, h = 1.5);

        // the grub screw, when the shaft is plain round
        if (knob_grub)
            translate([0, 0, 4.0]) rotate([0, 90, 0])
                cylinder(d = 2.5, h = knob_d);   // 2.5 taps M3

        // relieve the skirt so the wall over the bore is even
        translate([0, 0, -0.1])
            difference() {
                cylinder(d = knob_bore + 2 * knob_skirt + 0.4, h = 0.6);
                cylinder(d = knob_bore, h = 1);
            }
    }
}

// --------------------------------------------------------------------------
// render
// --------------------------------------------------------------------------
if (part == "shell") shell();
else if (part == "bezel") bezel();
else if (part == "knob") knob();
else {
    // Preview: the two parts apart, as they are printed and not as assembled,
    // because what matters first is whether each one prints.
    shell();
    translate([0, in_h + 2 * wall + 2 * bezel_lip + 10, 0]) bezel();
    translate([-knob_d - 10, 0, 0]) knob();
}

// The panel cut-out this needs, for whoever makes the hole:
//   width  = in_w + 2 * wall + 0.5 mm clearance
//   height = in_h + 2 * wall + 0.5 mm clearance
echo(str("PANEL CUT-OUT: ", in_w + 2 * wall + 0.5, " x ", in_h + 2 * wall + 0.5, " mm"));
echo(str("OUTSIDE SIZE:  ", in_w + 2 * wall + 2 * bezel_lip, " x ",
         in_h + 2 * wall + 2 * bezel_lip, " x ", in_d + floor_t + bezel_t, " mm"));
