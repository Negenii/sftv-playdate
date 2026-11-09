"""The console silhouette for SFTV RX (playdate/Source/images/console.png, 140x118, 1 bit):
the receiver in the receive scene, on the launcher card and in the launch animation. Drawn
here from primitives. The screen interior must stay at (7, 8), 88x53: main.c lights up
(7, 7, 88, 53) (CONSOLE_SCREEN_*) and tools/make_launcher.py puts pictures there.
Usage: python3 tools/make_console.py   (from the repo root; needs Pillow)"""
from PIL import Image, ImageDraw

W, H = 140, 118
K, Wh = 0, 255
im = Image.new("L", (W, H), Wh)
d = ImageDraw.Draw(im)


def disk(cx, cy, r, color):
    """A round disc set pixel by pixel (PIL's small ellipses come out as octagons)."""
    for y in range(int(cy - r) - 1, int(cy + r) + 2):
        for x in range(int(cx - r) - 1, int(cx + r) + 2):
            if 0 <= x < W and 0 <= y < H and (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                im.putpixel((x, y), color)


# body
d.rounded_rectangle([0, 1, 112, 117], 5, fill=K)
d.line([(84, 0), (95, 0)], fill=K)                   # the lock button on the top edge
# crank, seen from the front: a housing down the right edge (its foot joins the body) and the
# arm out of its top; a 1 px gap separates them from the body above the foot
d.rounded_rectangle([110, 22, 121, 63], 5, fill=K)   # housing
d.rounded_rectangle([114, 22, 139, 33], 3, fill=K)   # arm
d.line([(123, 21), (138, 21)], fill=K)               # the arm's top edge, past the housing
d.line([(113, 21), (113, 53)], fill=Wh)              # the gap
# screen: a 5 px white bezel, the dark (off) screen inside
d.rounded_rectangle([2, 3, 99, 64], 4, fill=Wh)
d.rectangle([7, 8, 94, 60], fill=K)
# the crank's slot in the body's side: a white frame with a dark line in it
d.rounded_rectangle([105, 28, 108, 46], 1, fill=Wh)
d.line([(107, 29), (107, 44)], fill=K)
# d-pad
d.rounded_rectangle([22, 76, 30, 105], 4, fill=Wh)
d.rounded_rectangle([12, 87, 41, 94], 3, fill=Wh)
# B and A
disk(67.5, 83, 6.8, Wh)
disk(90, 83, 6.6, Wh)
# screws: white rings
for sx, sy in ((106.5, 7), (6, 111.5), (106.5, 111.5)):
    disk(sx, sy, 3.9, Wh)
    disk(sx, sy, 1.6, K)

im.convert("1").save("playdate/Source/images/console.png")
print("ok")
