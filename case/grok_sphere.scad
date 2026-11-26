// Grok Sphere enclosure for the Waveshare ESP32-S3-Touch-LCD-1.85C (V2).  Units: mm.
// Designed 2026-11-25 for a Creality K2 Plus (PLA, 0.4 nozzle, 0.2 layers), no supports.
//
// Look: the Grok Bot Sphere reference (Texas_Twins_Dad, Nov 2026): a seamless glossy black ball, the face a flat
// circular opening with a thin lip ring and a near-flush screen, honeycomb grille low, short dark base.
// Build: like his printed shell, the BODY is one piece printed upright on its flat floor. The board goes in through
// the face and is clamped by a twist-lock BEZEL RING. The floor is open: you reach in from below to plug the
// right-angle USB-C into the board; the twist-lock BASE closes it, carries the speaker and lets the cable out.
//
// Device coordinates: sphere centre at origin, screen faces -Y, up is +Z. The finished device leans back by `tilt`
// so the face looks slightly up; the flat floor is cut in WORLD coordinates.
// Board: ESP32-S3-Touch-LCD-1.85C-BOX (Waveshare outline drawing, product page "outline dimensions"): a puck D58 x
// 48.6 deep (51.1 over the side buttons), touch area D46.08, speaker grille D46 on the BACK face, USB-C on the side
// of the board section near the SCREEN (~8.5 mm behind the glass). The puck sits with its USB-C pointing
// DOWN; a right-angle plug fits under it inside the collar slot. Firmware rotates the screen to suit.
//
// Parts:  openscad -D 'part="body"' -o body.stl grok_sphere.scad    (body | bezel | base)
part = "assembly";

// ---------------- parameters ----------------
R          = 52;     // sphere radius (D104)
wall       = 2.6;
tilt       = 10;     // face looks up by this many degrees
floor_z    = -45;    // world-Z of the flat floor

board_d    = 58.0;   board_t = 48.6;   va_d = 46.08;   // puck: screen glass to back face
usb_y      = 8.5;    // USB-C centre, mm behind the screen (board section, near the screen)
usb_w      = 15;     usb_t = 11;    usb_drop = 10.5;   // right-angle plug clearance: width, thickness, drop below the puck edge
y_face     = -38.5;  // face plane: opening D = 2*sqrt(R^2 - y_face^2) ~ 70
foam       = 1.0;    // foam ring behind the board takes up tolerance

// bezel ring (the lip ring)
ring_gap   = 0.6;    // radial gap between the ring and the face opening (reads as a fine dark line)
ring_set   = 0.8;    // ring face sits this far behind the face plane
ring_t     = 2.4;    // ring plate thickness
aperture_d = 48.6;   // visible opening (> va_d)
chamfer    = 1.0;
skirt_od   = 66.0;   skirt_id = 63.6;  skirt_l = 6.0;    // skirt carries the lugs; ID clears the side buttons
lug_w      = 9;      lug_h = 2.4;      lug_dr = 0.9;     twist = 25;   clr = 0.3;   // lug_dr 0.9 keeps the entry notches hidden behind the bezel plate
halo_n     = 44;     halo_r = 30.6;    halo_d = 1.1;     // mic pinholes in the ring, over the gap around the puck rim

// collar inside the body
bore_d     = 58.6;   // main bore (board frame, 0.3 radial clearance)
cbore_d    = skirt_od + 2*clr;          // counterbore for the ring skirt
collar_l   = 12;     // the collar centres the front 12 mm of the puck
collar_od  = 74;
stop_r     = 25.5;   // rear stop ribs reach in to this radius (outside the D46 speaker grille)



// floor opening + base
open_d     = 40.0;   // bore in the floor collar (base spigot goes in here)
floor_ring_h = 4.0;  // floor collar height (keeps clear of the grille)
base_d     = 50;     base_h = 14;   spigot_h = 5.5;
blug_w     = 8;      blug_h = 2.2;  blug_dr = 1.2;
cable_w    = 9;      cable_h = 8;                       // cable channel under the base, out to the back
plug_hole  = [17, 10];                                  // the cable's far plug end passes down through the base here
cable_y    = -9;                                        // world-y of that slot (front part of the base, under the cable)
weight_d   = 34;     weight_h = 5;  felt_d = 46;  felt_h = 1.0;

hole_d     = 2.4;    grille_rows = 4;  grille_pitch = 3.5;   // honeycomb grille band

$fn = 120;

face_r  = sqrt(R*R - y_face*y_face);
y_ring  = y_face + ring_set;           // ring front face
y_glass = y_ring + ring_t;             // board front (glass) rests on the ring's back face
y_bback = y_glass + board_t;           // board back
y_stop  = y_bback + foam;              // stop ring front face
usb_world_y = (y_glass + usb_y) * cos(tilt) - (board_d/2) * sin(tilt);   // world-y of the USB-C port (it points down)
echo(face_d = 2*face_r, y_glass = y_glass, y_puck_back = y_bback, usb_world_y = usb_world_y);

// ---------------- helpers ----------------
module ycyl(d, y0, y1) { translate([0, y0, 0]) rotate([-90, 0, 0]) cylinder(d = d, h = y1 - y0); }
module ycone(d0, d1, y0, y1) { translate([0, y0, 0]) rotate([-90, 0, 0]) cylinder(d1 = d0, d2 = d1, h = y1 - y0); }
module wz() { rotate([tilt, 0, 0]) children(); }                 // world-vertical frame placed in device coords
module above_floor(dz = 0) { wz() translate([-300, -300, floor_z + dz]) cube(600); }
module teardrop_bore(d, y0, y1) {   // circle with a 45-degree pointed roof: prints without support
  translate([0, y0, 0]) rotate([-90, 0, 0]) linear_extrude(y1 - y0)
    intersection() {   // 2D -y = device +Z: pointed roof, capped 2.5 mm above the circle so it stays inside the collar
      union() { circle(d = d); polygon([[-d/2*cos(45), -d/2*sin(45)], [0, -d/2*sqrt(2)], [d/2*cos(45), -d/2*sin(45)]]); }
      translate([-d, -(d/2 + 2.5)]) square([2*d, d + 2.5 + d/2]);
    }
}
module lugs(r, w, h, dr, y0, y1, a0 = 0) {   // 3 radial lugs on a cylinder of radius r, between y0..y1
  for (i = [0:2]) rotate([0, a0 + i*120, 0]) intersection() {
    difference() { ycyl(2*(r + dr), y0, y1); ycyl(2*(r - 0.6), y0 - 1, y1 + 1); }
    translate([-w/2, y0 - 1, 0]) cube([w, y1 - y0 + 2, r + dr + 1]);
  }
}
module bayonet_slots(r, w, h, dr, y_entry, y_lug0, y_lug1, a0 = 0) {   // entry notch + twist groove (turn clockwise from the front)
  for (i = [0:2]) rotate([0, a0 + i*120, 0]) {
    intersection() { ycyl(2*(r + dr + clr + 0.2), y_entry, y_lug1 + clr); translate([-(w + 1)/2, y_entry - 1, 0]) cube([w + 1, y_lug1 - y_entry + 2, r + dr + 2]); }
    intersection() { ycyl(2*(r + dr + clr + 0.2), y_lug0 - clr, y_lug1 + clr);
                     hull() for (a = [0, twist]) rotate([0, -a, 0]) translate([-(w + 1)/2, y_lug0 - 2, 0]) cube([w + 1, y_lug1 - y_lug0 + 4, r + dr + 2]); }
  }
}

// ---------------- body ----------------
module outer_body() { intersection() { sphere(R); above_floor(); translate([-300, y_face, -300]) cube(600); } }
module inner_void() { intersection() { sphere(R - wall); above_floor(-1); } }   // floor is open (base closes it)

module collar() {   // ring around the front of the puck
  intersection() { ycyl(collar_od, y_face, y_glass + collar_l); sphere(R - 0.01); }
}
module collar_foot() {   // fills under the collar down to the shell so the collar never starts printing in mid-air
  intersection() {
    translate([-collar_od/2, y_face, -R]) cube([collar_od, (y_glass + collar_l) - y_face, R]);
    sphere(R - 0.01);
  }
}
module rear_stops() {   // 2 ribs from the lower shell to the puck back rim (+ centring lips). No top rib: it would hang from the dome and print in mid-air
  for (a = [120, 240]) rotate([0, a, 0]) {
    translate([-2, y_stop, stop_r]) cube([4, 3, R]);                                  // stop face (presses through the foam)
    translate([-2, y_bback - 6, (board_d + 0.6)/2]) cube([4, 6 + foam + 3, R]);      // centring lip behind the rim
  }
}
module floor_collar() {   // thick ring around the floor opening for the base bayonet
  intersection() { wz() translate([0, 0, floor_z]) cylinder(d = 2*sqrt(R*R - floor_z*floor_z) + 2, h = floor_ring_h); sphere(R - 0.01); }
}
module collar_cuts() {
  ycyl(cbore_d, y_face - 1, y_ring + ring_t + skirt_l + 0.4);       // counterbore for the ring + skirt
  teardrop_bore(bore_d, y_face, y_glass + collar_l + 0.01);           // front bore (pointed roof)
  // right-angle USB-C plug: slot down through the collar at 6 o'clock, open to the cavity behind
  translate([-usb_w/2, y_glass + usb_y - usb_t/2, -(board_d/2 + usb_drop)]) cube([usb_w, usb_t + 30, usb_drop + 4]);
  bayonet_slots(skirt_od/2, lug_w, lug_h, lug_dr, y_face - 1, y_ring + ring_t + skirt_l - lug_h - 0.5, y_ring + ring_t + skirt_l - 0.5, 60);
}
module rear_grille() {   // honeycomb patch on the back, behind the puck's speaker (it fires backward)
  for (gx = [-8 : 8]) for (gz = [-8 : 8]) {
    x = (gx + (abs(gz) % 2) * 0.5) * grille_pitch; z = gz * grille_pitch * 0.866;
    if (x*x + z*z < 20*20) translate([x, 0, z]) ycyl(hole_d, sqrt(max(0, R*R - x*x - z*z)) - wall - 2, R + 2);
  }
}
module grille() {
  for (r = [0 : grille_rows - 1]) {
    zr = floor_z + floor_ring_h + 3 + r * grille_pitch * 0.866;
    rr = sqrt(max(0, R*R - zr*zr));
    n  = floor(2 * PI * rr / grille_pitch);
    for (i = [0 : n - 1]) wz() rotate([0, 0, (i + (r % 2) * 0.5) * 360 / n])
      translate([0, 0, zr]) rotate([-90, 0, 0]) cylinder(d = hole_d, h = R + 2, $fn = 10);
  }
}
module body() {
  difference() {
    union() {
      difference() { outer_body(); inner_void(); }
      intersection() { union() { collar(); collar_foot(); floor_collar(); rear_stops(); } outer_body(); }
    }
    collar_cuts();
    grille();
    rear_grille();
    wz() translate([0, 0, floor_z - 1]) cylinder(d = open_d + 2*clr, h = floor_ring_h + 2);          // floor opening
    wz() bayonet_slots_v(open_d/2, blug_w, blug_dr);
  }
}
module bayonet_slots_v(r, w, dr) {   // same bayonet, but on a world-vertical axis (base enters from below)
  for (i = [0:2]) rotate([0, 0, i*120 + 90]) {
    intersection() { translate([0, 0, floor_z - 1]) cylinder(r = r + dr + clr + 0.2, h = floor_ring_h - 1 + 1); translate([-(w + 1)/2, 0, floor_z - 1]) cube([w + 1, r + dr + 2, floor_ring_h]); }
    intersection() { translate([0, 0, floor_z + floor_ring_h - 1.2 - blug_h - clr]) cylinder(r = r + dr + clr + 0.2, h = blug_h + 2*clr);
                     hull() for (a = [0, twist]) rotate([0, 0, -a]) translate([-(w + 1)/2, 0, floor_z]) cube([w + 1, r + dr + 2, floor_ring_h]); }
  }
}

// ---------------- bezel ring ----------------
module bezel() {   // modelled in place (device coords)
  difference() {
    union() {
      ycyl(2*(face_r - ring_gap), y_ring, y_ring + ring_t);                         // plate
      ycyl(skirt_od, y_ring + ring_t - 0.01, y_ring + ring_t + skirt_l);           // skirt
      lugs(skirt_od/2, lug_w, lug_h, lug_dr, y_ring + ring_t + skirt_l - lug_h - 0.5, y_ring + ring_t + skirt_l - 0.5, 60);
    }
    ycyl(skirt_id, y_ring + ring_t, y_ring + ring_t + skirt_l + 1);                 // skirt bore (clears the board edge)
    ycyl(aperture_d, y_ring - 1, y_ring + ring_t + 1);                              // aperture
    ycone(aperture_d + 2*chamfer, aperture_d, y_ring - 0.01, y_ring + chamfer);     // chamfer
    for (i = [0 : halo_n - 1]) rotate([0, i * 360 / halo_n + 4.5, 0]) translate([halo_r, 0, 0]) ycyl(halo_d, y_ring - 1, y_ring + ring_t + 1);
    // two finger notches on the ring edge to twist it (at 3 and 9 o'clock, shallow)
    for (s = [-1, 1]) translate([s*(face_r - ring_gap), y_ring - 0.01, 0]) scale([1, 1, 2.2]) rotate([-90,0,0]) cylinder(d = 3, h = 1.0, $fn = 24);
  }
}

// ---------------- base ----------------
module base() {   // modelled in place in the world-vertical frame (call inside wz())
  difference() {
    union() {
      translate([0, 0, floor_z - base_h]) cylinder(d = base_d, h = base_h);                     // short dark puck below the floor
      translate([0, 0, floor_z - 0.01]) cylinder(d = open_d, h = spigot_h);                     // spigot into the floor opening
      for (i = [0:2]) rotate([0, 0, i*120 + 90]) intersection() {                               // bayonet lugs
        translate([0, 0, floor_z + floor_ring_h - 1.2 - blug_h]) difference() { cylinder(r = open_d/2 + blug_dr, h = blug_h); translate([0,0,-1]) cylinder(r = open_d/2 - 1, h = blug_h + 2); }
        translate([-blug_w/2, 0, floor_z]) cube([blug_w, open_d, floor_ring_h + 2]);
      }
    }
    // straight USB-C: the plug drops through the base under the puck's port, then the cable runs back in a channel
    translate([-plug_hole[0]/2, cable_y - plug_hole[1]/2, floor_z - base_h - 1]) cube([plug_hole[0], plug_hole[1], base_h + spigot_h + 2]);
    translate([-cable_w/2, cable_y, floor_z - base_h - 0.01]) cube([cable_w, base_d, cable_h]);
    translate([0, 0, floor_z - base_h - 0.01]) cylinder(d = felt_d, h = felt_h);                            // felt pad
    for (sx = [-1, 1]) translate([sx * 14, cable_y + 6, floor_z - base_h - 0.01]) cylinder(d = 14, h = weight_h + felt_h);   // two steel weight pockets beside the plug
  }
}

// ---------------- previews / checks ----------------
module board_ghost() {
  color("royalblue") ycyl(board_d, y_glass, y_bback);
  color("#0b0b0b") ycyl(va_d, y_glass - 0.4, y_glass);
  color("silver") translate([-4.4, y_glass + usb_y - 1.6, -board_d/2 - 6]) cube([8.8, 3.2, 6]);                      // plug neck
  color("#333") translate([-usb_w/2 + 1, y_glass + usb_y - usb_t/2 + 0.5, -board_d/2 - usb_drop + 0.5]) cube([usb_w - 2, usb_t - 1, usb_drop - 6]);   // right-angle overmold
  color("#333") translate([-2.2, y_glass + usb_y, -board_d/2 - usb_drop + 1]) cube([4.4, 40, 4.4]);                 // cable heading back
}
module world() { rotate([-tilt, 0, 0]) children(); }
module dcut() { projection(cut = true) rotate([0, 90, 0]) children(); }

if (part == "body")        world() body();                                           // sits on its floor (z = floor_z)
else if (part == "bezel")  rotate([90, 0, 0]) translate([0, -y_ring, 0]) bezel();    // ring face on the bed
else if (part == "base")   base();                                                   // upright, bottom on the bed
else if (part == "fit_test") rotate([90, 0, 0]) translate([0, -y_face, 0])   // front of the body, face-down: tests the puck in the collar + bezel bayonet
                             intersection() { body(); translate([-100, -100, -100]) cube([200, 100 + y_glass + collar_l + 2, 200]); }
else if (part == "cut_body")  dcut() body();
else if (part == "cut_bezel") dcut() bezel();
else if (part == "cut_base")  dcut() wz() base();
else if (part == "cut_board") dcut() board_ghost();
else if (part == "assembly") world() { color("#2b2b2e") body(); color("#45454a") bezel(); color("#1d1d1f") wz() base(); }

// ---- assembled-position exports for visual QC (world coordinates) ----
if (part == "asm_body")  world() body();
if (part == "asm_bezel") world() bezel();
if (part == "asm_base")  world() wz() base();
if (part == "asm_puck")  world() board_ghost();

if (part == "floor_slice") projection(cut = true) translate([0, 0, -(floor_z + 2)]) world() body();   // horizontal slice 2 mm above the floor
