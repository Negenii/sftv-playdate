"""The console silhouette for SFTV RX (playdate/Source/images/console.png, 140x118, 1 bit):
the receiver in the receive scene, on the launcher card and in the launch animation. Drawn
here from primitives. The screen interior must stay at (7, 7), 88x53: main.c lights it up
(CONSOLE_SCREEN_*) and tools/make_launcher.py puts pictures there.
Usage: python3 tools/make_console.py   (from the repo root; needs Pillow)"""
from PIL import Image, ImageDraw

W, H = 140, 118
K, Wh = 0, 255
im = Image.new("L", (W, H), Wh)
d = ImageDraw.Draw(im)

# crank, seen from the front: a housing down the right edge of the body and the arm sticking
# out of its top end (the knob points at the viewer, so it is not seen)
d.rounded_rectangle([115, 16, 123, 62], 3, fill=K)   # housing, 2 px clear of the body
d.rounded_rectangle([115, 18, 139, 29], 3, fill=K)   # arm
# body
d.rounded_rectangle([0, 0, 112, 117], 7, fill=K)
# screen: white bezel line, black (off) interior at (7, 7) 88x53
d.rounded_rectangle([3, 3, 98, 63], 5, outline=Wh, width=2)
d.rectangle([7, 7, 94, 59], fill=K)
# the crank's slot in the body's side
d.rounded_rectangle([105, 26, 107, 46], 1, fill=Wh)
# d-pad
cx, cy, arm, th = 25, 89, 12, 7
d.rounded_rectangle([cx - arm, cy - th // 2, cx + arm, cy + th // 2], 2, fill=Wh)
d.rounded_rectangle([cx - th // 2, cy - arm, cx + th // 2, cy + arm], 2, fill=Wh)
# B and A
for bx in (67, 89):
    d.ellipse([bx - 6, 83 - 6, bx + 6, 83 + 6], fill=Wh)
# screws
for sx, sy in ((106, 6), (106, 110), (6, 110)):
    d.ellipse([sx - 3, sy - 3, sx + 3, sy + 3], outline=Wh, width=1)

im.convert("1").save("playdate/Source/images/console.png")
print("ok")
