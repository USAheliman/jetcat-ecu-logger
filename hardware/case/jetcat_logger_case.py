# JetCat ECU Logger - XIAO board v3 case (CadQuery). GPL-3.0-or-later.
#
# Two parts: BASE (floor + walls) and LID. The board stands on its own sticky-pad nylon
# standoffs, stuck to the floor. 4 x M2 screws through the lid into M2 heat-set inserts
# (OD 3.2, length 4) in the base bosses.
#
# Coordinates (top view, like the board drawing JetCat_Logger_XIAO_board_v3):
#   +Y = USB-C end (row A), -Y = RJ12 end (row T); -X = column 1 side, +X = column 14 side.
#   Z = 0 at the top of the floor (= bottom of the standoff pads).
# Heights measured by Mehul, 12 Nov 2026 (from the bottom of the standoff pads):
#   footprint incl. pads 70 x 50; LED tops 30; RJ12 top 27; RJ12 bottom 10; button top 25;
#   USB-C centre 24. Hole positions from the 2.54 mm grid of the 40 x 60 board.
import cadquery as cq

PITCH = 2.54
def col_x(c): return (c - 7.5) * PITCH          # column 1..14
def row_y(r): return 24.13 - r * PITCH           # row index, A = 0 .. T = 19

# ---- main dimensions
IN_X, IN_Y = 50.6, 70.6        # inside: 50 x 70 footprint + 0.3 clearance each side
WALL = 2.2
FLOOR = 2.0
IN_H = 28.0                    # floor top to lid underside (RJ12 top 27 + 1)
LID_T = 2.0                    # lid top = 30 = LED tops, so the LEDs sit flush
OUT_X, OUT_Y = IN_X + 2 * WALL, IN_Y + 2 * WALL
R_OUT = 3.0

# ---- features (board coordinates)
LEDS = [("PWR", row_y(0)), ("ECU", row_y(3)), ("DATA", row_y(6))]   # red, yellow, green
LED_X = col_x(11.5)
LED_HOLE = 5.5                 # 5 mm LEDs
BTN_X, BTN_Y = col_x(2), row_y(2.5)  # SW1 centre
BTN_TOP = 25.0
USB_X, USB_Z, USB_W, USB_H = col_x(7), 24.0, 13.0, 7.5   # room for the plug's moulding
JACK_X, JACK_Z0, JACK_W = col_x(9), 10.0 - 0.3, 15.0     # RJ12 face, open to the lid
BOSS_D, INSERT_D, INSERT_DEPTH = 5.5, 3.2, 5.0
BOSSES = [(sx * (IN_X / 2 - 2.0), sy * 15.0) for sx in (-1, 1) for sy in (-1, 1)]
SCREW_CLEAR, CSK_D = 2.4, 4.2

# ---------------------------------------------------------------- base
base = (cq.Workplane("XY").rect(OUT_X, OUT_Y).extrude(FLOOR + IN_H)
        .edges("|Z").fillet(R_OUT)
        .faces("<Z").edges().chamfer(0.6))
pocket = (cq.Workplane("XY").workplane(offset=FLOOR).rect(IN_X, IN_Y).extrude(IN_H + 1)
          .edges("|Z").fillet(0.8))
base = base.cut(pocket)
for (x, y) in BOSSES:
    boss = cq.Workplane("XY").workplane(offset=FLOOR).center(x, y).circle(BOSS_D / 2).extrude(IN_H)
    # merge each boss into the side wall
    web = (cq.Workplane("XY").workplane(offset=FLOOR)
           .center((x + (IN_X / 2 + 0.5) * (1 if x > 0 else -1)) / 2, y)
           .rect(abs((IN_X / 2 + 0.5) - abs(x)), BOSS_D).extrude(IN_H))
    base = base.union(boss).union(web)
    base = base.cut(cq.Workplane("XY").workplane(offset=FLOOR + IN_H - INSERT_DEPTH)
                    .center(x, y).circle(INSERT_D / 2).extrude(INSERT_DEPTH + 1))
# USB-C notch in the +Y wall, open at the top (the lid closes it)
z0 = FLOOR + USB_Z - USB_H / 2
base = base.cut(cq.Workplane("XY").workplane(offset=z0)
                .center(USB_X, IN_Y / 2 + WALL / 2).rect(USB_W, WALL * 3).extrude(IN_H + 5))
# RJ12 notch in the -Y wall, open at the top
base = base.cut(cq.Workplane("XY").workplane(offset=FLOOR + JACK_Z0)
                .center(JACK_X, -(IN_Y / 2 + WALL / 2)).rect(JACK_W, WALL * 3).extrude(IN_H + 5))

# ---------------------------------------------------------------- lid
lid = (cq.Workplane("XY").rect(OUT_X, OUT_Y).extrude(LID_T)
       .edges("|Z").fillet(R_OUT)
       .faces(">Z").edges().chamfer(0.6))
# screw holes, countersunk on top
for (x, y) in BOSSES:
    lid = lid.cut(cq.Workplane("XY").center(x, y).circle(SCREW_CLEAR / 2).extrude(LID_T))
    lid = lid.cut(cq.Solid.makeCone(0, CSK_D / 2 + 0.1, CSK_D / 2 + 0.1,
                                    cq.Vector(x, y, LID_T - (CSK_D / 2 + 0.1)), cq.Vector(0, 0, 1)))
# LED holes + labels
for name, y in LEDS:
    lid = lid.cut(cq.Workplane("XY").center(LED_X, y).circle(LED_HOLE / 2).extrude(LID_T))
    txt = (cq.Workplane("XY").workplane(offset=LID_T - 0.6)
           .text(name, 3.2, 1.0, halign="left", valign="center", kind="bold")
           .translate((LED_X + 4.0, y, 0)))
    lid = lid.cut(txt)
# title
lid = lid.cut(cq.Workplane("XY").workplane(offset=LID_T - 0.6)
              .text("JetCat Logger", 5.0, 1.0, halign="center", valign="center", kind="bold")
              .translate((0, -18, 0)))
lid = lid.cut(cq.Workplane("XY").workplane(offset=LID_T - 0.6)
              .text("USB-C", 2.8, 1.0, halign="center", valign="top")
              .translate((USB_X, OUT_Y / 2 - 1.8, 0)))
lid = lid.cut(cq.Workplane("XY").workplane(offset=LID_T - 0.6)
              .text("ECU", 2.8, 1.0, halign="center", valign="bottom")
              .translate((JACK_X, -OUT_Y / 2 + 1.8, 0)))

# flexure button over SW1: a pad on a tongue, cut free by a 1 mm U-slot, thinned to 1.0 mm
PAD_D, TONGUE_W, TONGUE_L, SLOT = 8.0, 8.0, 14.0, 1.0
tongue_root_y = BTN_Y - TONGUE_L          # hinge towards the middle of the lid
u = (cq.Workplane("XY").center(BTN_X, (BTN_Y + tongue_root_y) / 2 + PAD_D / 4)
     .rect(TONGUE_W + 2 * SLOT, TONGUE_L + PAD_D / 2 + SLOT).extrude(LID_T)
     .cut(cq.Workplane("XY").center(BTN_X, (BTN_Y + tongue_root_y) / 2 + PAD_D / 4 - SLOT / 2)
          .rect(TONGUE_W, TONGUE_L + PAD_D / 2).extrude(LID_T)))
lid = lid.cut(u)
# thin the tongue from below (keep the pad full thickness)
thin = (cq.Workplane("XY").center(BTN_X, (BTN_Y - PAD_D / 2 + tongue_root_y) / 2)
        .rect(TONGUE_W + 0.2, (BTN_Y - PAD_D / 2) - tongue_root_y).extrude(LID_T - 1.0))
lid = lid.cut(thin)
# plunger down to 0.4 mm above the button top (lid underside is at IN_H)
plunger_len = IN_H - (BTN_TOP + 0.4)
lid = lid.union(cq.Workplane("XY").workplane(offset=-plunger_len)
                .center(BTN_X, BTN_Y).circle(2.0).extrude(plunger_len + 0.01))
# finger dimple on the pad
lid = lid.cut(cq.Workplane("XY").workplane(offset=LID_T - 0.5).center(BTN_X, BTN_Y)
              .circle(3.0).extrude(1))

if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    cq.exporters.export(base, f"{out}/JetCat_Logger_case_base.stl", tolerance=0.02, angularTolerance=0.1)
    # lid printed upside down (top face on the bed): flip it
    lid_print = lid.rotate((0, 0, 0), (0, 1, 0), 180).translate((0, 0, LID_T))
    cq.exporters.export(lid_print, f"{out}/JetCat_Logger_case_lid.stl", tolerance=0.02, angularTolerance=0.1)
    asm = (cq.Assembly()
           .add(base, name="base", color=cq.Color(0.15, 0.15, 0.18))
           .add(lid, name="lid", loc=cq.Location((0, 0, FLOOR + IN_H)), color=cq.Color(0.85, 0.85, 0.88)))
    asm.save(f"{out}/JetCat_Logger_case.step")
    print("bbox base", base.val().BoundingBox().xlen, base.val().BoundingBox().ylen, base.val().BoundingBox().zlen)
    print("bbox lid", lid.val().BoundingBox().xlen, lid.val().BoundingBox().ylen, lid.val().BoundingBox().zlen)
    print("solids", len(base.solids().vals()), len(lid.solids().vals()))
