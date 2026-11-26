# Grok Sphere enclosure

Source: `grok_sphere.scad` (OpenSCAD). STLs go in `stl/`.

The look follows the Grok Bot Sphere reference (Texas_Twins_Dad, Nov 2026): a seamless black ball with a flat
circular face, a thin lip ring around a near-flush screen, a honeycomb grille low on the ball and a short dark base.
It is 104 mm across, sized for the **ESP32-S3-Touch-LCD-1.85C-BOX** puck (D58 x 48.6 mm, speaker firing out the back,
USB-C on the side of the board section, near the screen). The face opening is 70 mm. The ball leans back 10° so it looks up slightly at
someone sitting at a desk.

## Parts (Creality K2 Plus, PLA, 0.4 mm nozzle, 0.2 mm layers, black PLA from CFS slot 1D)

| Part | Print orientation | Supports | Notes |
|---|---|---|---|
| `body.stl` | upright, on its flat floor (STL already placed) | none | one piece. The front collar's bore has a capped pointed roof so it bridges cleanly. Honeycomb grille low all round, plus a patch on the back behind the puck's speaker. 3 walls, 15% gyroid. A brim helps: the floor ring is narrow. |
| `bezel.stl` | face down | none | the lip ring around the screen, with the mic pinholes. The two small notches at 3 and 9 o'clock are finger grips for twisting it. Print it slowly for a clean face. |
| `base.stl` | upright | none | twist-locks into the floor. The USB-C plug drops through it; the cable runs out the back in a channel. Two steel weight pockets, felt recess. |
| `fit_test.stl` | face down | none | optional: just the front of the body (face + collar), ~2 h. Print it with the bezel first to check the puck fit and the bayonet. |

## Extra parts

- **Right-angle USB-C cable.** The puck's USB-C is near the screen, inside the front collar, so a straight plug won't
  fit. Use a 90° plug whose body sticks out no more than ~10 mm and is under ~14 mm wide. The cable runs back along
  the floor and down through the base.
- **Foam:** 1 mm craft foam or EVA. Three small pads (about 5 x 8 mm) go on the rear ribs where they meet the
  puck's back rim. They take up tolerance and stop rattles.
- **Steel weight:** two 14 mm discs (stacked washers or slugs) up to 5 mm tall, glued into the base pockets.
- **Felt pad:** 46 mm circle on the bottom of the base, with a notch for the cable channel.

## Assembly

1. Check the puck's power switch is ON, since it isn't reachable once it's inside.
2. Stick the foam pads on the three rear ribs inside the body.
3. Slide the puck in through the face, screen out, **USB-C at the bottom** (6 o'clock). It goes in until its back
   rim sits on the ribs.
4. Put the bezel ring in, lugs into the notches, and twist it clockwise about 25° until it clicks. The puck is now
   held between the bezel and the ribs.
5. Turn the ball over. Reach in through the floor opening and plug the right-angle USB-C into the puck through the
   slot at the bottom of the collar, with the cable heading back toward the opening.
6. Thread the cable's far end down through the slot in the base, lay it in the channel so it leaves at the back, and twist
   the base into the floor (lugs into the notches, then clockwise).
7. Glue in the weights, stick on the felt pad, and plug it in.

If the eyes come up rotated, the screen rotation is set in firmware (`ui.cpp`).

## Fit checks to do on the first print

- The puck slides into the collar with a light push (0.3 mm per side), and the bezel twists in without forcing over
  the side buttons.
- The mic pinholes line up over the gap around the puck's rim. The wake word should work through them; test with
  "Hi ESP" before closing the base.
- If anything is tight, the parameters at the top of `grok_sphere.scad` control it: `bore_d`, `clr`, `foam`,
  `skirt_id`, `base_h`.
