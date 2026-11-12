# JetCat ECU Logger - XIAO board v3 case (CadQuery). GPL-3.0-or-later.
#
# Two parts: a flat BASE PLATE that the board stands on (its sticky-pad nylon standoffs are
# stuck to the plate), and a COVER (walls + top in one piece) that drops over the board.
# 4 x M2 countersunk screws (M2 x 8) from underneath, through the plate into M2 heat-set
# inserts (OD 3.2, length 4) in the cover's bosses, so no screws show on top. The cover's
# walls sit in a 1.2 mm groove in the plate.
#
# Coordinates (top view, like the board drawing JetCat_Logger_XIAO_board_v3):
#   +Y = USB-C end (row A), -Y = RJ12 end (row T); -X = column 1 side, +X = column 14 side.
#   Z = 0 at the top of the base plate (= bottom of the standoff pads).
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

PLATE_T = 3.0                  # base plate; pads stick to its top (Z = 0)
GROOVE_D, GROOVE_CLR = 1.2, 0.2   # plate edge rebated 1.2 mm: the cover walls sit on it, the raised middle locates them
COVER_H = IN_H + LID_T         # 30: top of the cover = LED tops

# ---------------------------------------------------------------- cover (walls + top, one piece)
cover = (cq.Workplane("XY").workplane(offset=-GROOVE_D).rect(OUT_X, OUT_Y)
         .extrude(COVER_H + GROOVE_D)
         .edges("|Z").fillet(R_OUT)
         .faces(">Z").edges().chamfer(0.6))
cover = cover.cut(cq.Workplane("XY").workplane(offset=-GROOVE_D - 1).rect(IN_X, IN_Y)
                  .extrude(IN_H + GROOVE_D + 1).edges("|Z").fillet(0.8))
for (x, y) in BOSSES:
    side = 1 if x > 0 else -1
    cover = cover.union(cq.Workplane("XY").center(x, y).circle(BOSS_D / 2).extrude(IN_H))
    cover = cover.union(cq.Workplane("XY").center((x + side * (IN_X / 2 + 0.5)) / 2, y)
                        .rect(abs((IN_X / 2 + 0.5) - abs(x)), BOSS_D).extrude(IN_H))
    cover = cover.cut(cq.Workplane("XY").workplane(offset=-1).center(x, y)
                      .circle(INSERT_D / 2).extrude(INSERT_DEPTH + 1))     # insert from below
# USB-C window (+Y wall) and RJ12 window (-Y wall), up to the inside of the top
cover = cover.cut(cq.Workplane("XY").workplane(offset=USB_Z - USB_H / 2)
                  .center(USB_X, IN_Y / 2 + WALL / 2).rect(USB_W, WALL * 3)
                  .extrude(IN_H - (USB_Z - USB_H / 2)))
cover = cover.cut(cq.Workplane("XY").workplane(offset=JACK_Z0)
                  .center(JACK_X, -(IN_Y / 2 + WALL / 2)).rect(JACK_W, WALL * 3)
                  .extrude(IN_H - JACK_Z0))

# top: LED holes and engraving
for name, y in LEDS:
    cover = cover.cut(cq.Workplane("XY").workplane(offset=IN_H - 1).center(LED_X, y)
                      .circle(LED_HOLE / 2).extrude(LID_T + 2))
    cover = cover.cut(cq.Workplane("XY").workplane(offset=COVER_H - 0.6)
                      .text(name, 3.2, 1.0, halign="left", valign="center", kind="bold")
                      .translate((LED_X + 4.0, y, 0)))
for t, size, pos, va in (("JetCat Logger", 5.0, (0, -18), "center"),
                         ("USB-C", 2.8, (USB_X, OUT_Y / 2 - 1.8), "top"),
                         ("ECU", 2.8, (JACK_X, -OUT_Y / 2 + 1.8), "bottom")):
    cover = cover.cut(cq.Workplane("XY").workplane(offset=COVER_H - 0.6)
                      .text(t, size, 1.0, halign="center", valign=va, kind="bold" if size > 4 else "regular")
                      .translate((pos[0], pos[1], 0)))

# flexure button over SW1: a pad on a tongue, cut free by a 1 mm U-slot, thinned to 1.0 mm
PAD_D, TONGUE_W, TONGUE_L, SLOT = 8.0, 8.0, 14.0, 1.0
root_y = BTN_Y - TONGUE_L              # hinge towards the middle
mid = (BTN_Y + root_y) / 2 + PAD_D / 4
u = (cq.Workplane("XY").workplane(offset=IN_H - 0.01).center(BTN_X, mid)
     .rect(TONGUE_W + 2 * SLOT, TONGUE_L + PAD_D / 2 + SLOT).extrude(LID_T + 1)
     .cut(cq.Workplane("XY").workplane(offset=IN_H - 0.01).center(BTN_X, mid - SLOT / 2)
          .rect(TONGUE_W, TONGUE_L + PAD_D / 2).extrude(LID_T + 1)))
cover = cover.cut(u)
cover = cover.cut(cq.Workplane("XY").workplane(offset=IN_H - 0.01)
                  .center(BTN_X, (BTN_Y - PAD_D / 2 + root_y) / 2)
                  .rect(TONGUE_W + 0.2, (BTN_Y - PAD_D / 2) - root_y).extrude(LID_T - 1.0 + 0.01))
plunger_len = IN_H - (BTN_TOP + 0.4)
cover = cover.union(cq.Workplane("XY").workplane(offset=BTN_TOP + 0.4)
                    .center(BTN_X, BTN_Y).circle(2.0).extrude(plunger_len + 0.01))
cover = cover.cut(cq.Workplane("XY").workplane(offset=COVER_H - 0.5).center(BTN_X, BTN_Y)
                  .circle(3.0).extrude(1))

# ---------------------------------------------------------------- base plate
plate = (cq.Workplane("XY").workplane(offset=-PLATE_T).rect(OUT_X, OUT_Y).extrude(PLATE_T)
         .edges("|Z").fillet(R_OUT)
         .faces("<Z").edges().chamfer(0.6))
groove = (cq.Workplane("XY").workplane(offset=-GROOVE_D).rect(OUT_X + 2 * GROOVE_CLR, OUT_Y + 2 * GROOVE_CLR)
          .extrude(GROOVE_D + 1).edges("|Z").fillet(R_OUT)
          .cut(cq.Workplane("XY").workplane(offset=-GROOVE_D - 1).rect(IN_X - 2 * GROOVE_CLR, IN_Y - 2 * GROOVE_CLR)
               .extrude(GROOVE_D + 3)))
plate = plate.cut(groove)
for (x, y) in BOSSES:
    plate = plate.cut(cq.Workplane("XY").workplane(offset=-PLATE_T).center(x, y)
                      .circle(SCREW_CLEAR / 2).extrude(PLATE_T))
    plate = plate.cut(cq.Solid.makeCone(CSK_D / 2 + 0.1, 0, CSK_D / 2 + 0.1,
                                        cq.Vector(x, y, -PLATE_T), cq.Vector(0, 0, 1)))

if __name__ == "__main__":
    import os
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out): os.remove(os.path.join(out, f))
    # print orientations: cover top-down on the bed, plate bottom-down
    cover_print = cover.rotate((0, 0, 0), (0, 1, 0), 180).translate((0, 0, COVER_H))
    plate_print = plate.translate((0, 0, PLATE_T))
    cq.exporters.export(cover_print, f"{out}/JetCat_Logger_case_cover.stl", tolerance=0.02, angularTolerance=0.1)
    cq.exporters.export(plate_print, f"{out}/JetCat_Logger_case_base_plate.stl", tolerance=0.02, angularTolerance=0.1)
    asm = (cq.Assembly()
           .add(plate, name="base_plate", color=cq.Color(0.15, 0.15, 0.18))
           .add(cover, name="cover", color=cq.Color(0.85, 0.85, 0.88)))
    asm.export(f"{out}/JetCat_Logger_case.step")
    for n, p in (("cover", cover), ("plate", plate)):
        bb = p.val().BoundingBox()
        print(n, round(bb.xlen, 2), round(bb.ylen, 2), round(bb.zlen, 2), "solids", len(p.solids().vals()))
