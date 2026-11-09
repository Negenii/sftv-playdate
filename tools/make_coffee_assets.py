"""Buy Me a Coffee assets for the About pages: the QR code (https://buymeacoffee.com/low.effort,
version 3 = 29 modules, ECC M, no quiet zone: the apps leave white around it) for the Playdate
(5 px per module) and the Flipper (2 px), and a cup glyph for the Playdate button.
Usage: python3 tools/make_coffee_assets.py   (from the repo root)"""
import qrcode
from PIL import Image, ImageDraw

URL = "https://buymeacoffee.com/low.effort"
q = qrcode.QRCode(version=3, error_correction=qrcode.constants.ERROR_CORRECT_M, border=0)
q.add_data(URL)
q.make(fit=False)
m = q.get_matrix()
n = len(m)
assert n == 29, n
for scale, path in ((5, "playdate/Source/images/qr_coffee.png"), (2, "flipper/images/qr_coffee.png")):
    im = Image.new("1", (n * scale, n * scale), 1)
    d = ImageDraw.Draw(im)
    for y in range(n):
        for x in range(n):
            if m[y][x]:
                d.rectangle([x * scale, y * scale, x * scale + scale - 1, y * scale + scale - 1], fill=0)
    im.save(path)

# the Buy Me a Coffee cup (takeaway cup: lid, tapered body, filled lower part), 20x28, black on
# transparent; the Playdate draws it white on the black button
W, H = 20, 28
cup = Image.new("RGBA", (W, H), (0, 0, 0, 0))
d = ImageDraw.Draw(cup)
K = (0, 0, 0, 255)
d.rectangle([5, 0, 14, 1], fill=K)            # lid dome
d.rectangle([1, 2, 18, 5], fill=K)            # lid rim
top, bot = 7, H - 1                           # body: x 2..17 at the top -> 5..14 at the bottom
fill_from = 13                                # the coffee
for y in range(top, bot + 1):
    t = (y - top) / (bot - top)
    l = round(2 + 3 * t); r = round(17 - 3 * t)
    if y >= fill_from or y == bot:
        d.line([(l, y), (r, y)], fill=K)
    else:
        d.line([(l, y), (l + 1, y)], fill=K)
        d.line([(r - 1, y), (r, y)], fill=K)
cup.save("playdate/Source/images/cup.png")
print("ok", n)
