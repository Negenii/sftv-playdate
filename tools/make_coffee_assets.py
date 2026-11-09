"""Buy Me a Coffee assets for the About pages: the QR code (https://buymeacoffee.com/low.effort,
version 3 = 29 modules, ECC M, no quiet zone: the apps leave white around it) for the Playdate
(5 px per module) and the Flipper (2 px), and the 1-bit Buy Me a Coffee button for the Playdate.
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

# The Buy Me a Coffee button (their yellow button, tools/assets/bmc-button.png) in 1 bit, 200 px
# wide: dark outlines and lettering stay black, the cup's inside white, the yellow becomes a
# 12.5% dot pattern with a 1 px white halo around the lines (the dots would stick to the
# lettering otherwise), rounded corners transparent, a 1 px black frame.
src = Image.open("tools/assets/bmc-button.png").convert("RGBA")
W = 200
H = round(W * src.height / src.width)
im = src.resize((W, H), Image.LANCZOS)
def is_yellow(p):
    r, g, b, a = p
    return r > 180 and g > 160 and b < 120
def is_dark(x, y):
    p = im.getpixel((x, y))
    return p[3] >= 128 and not is_yellow(p) and 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] < 110
def dots(x, y):  # 1 in 8
    return (x % 4 == 0 and y % 2 == 0) or (x % 4 == 2 and y % 2 == 1)
btn = Image.new("RGBA", (W, H), (0, 0, 0, 0))
for y in range(H):
    for x in range(W):
        p = im.getpixel((x, y))
        if p[3] < 128:
            continue
        if is_yellow(p):
            near = any(0 <= x + dx < W and 0 <= y + dy < H and is_dark(x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1))
            black = not near and dots(x, y)
        else:
            black = is_dark(x, y)
        btn.putpixel((x, y), (0, 0, 0, 255) if black else (255, 255, 255, 255))
R = max(3, round(40 * W / src.width))
mask = Image.new("L", (W, H), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, H - 1], R, fill=255)
btn.putalpha(Image.composite(btn.getchannel("A"), mask, mask))
frame = Image.new("L", (W, H), 0)
ImageDraw.Draw(frame).rounded_rectangle([0, 0, W - 1, H - 1], R, outline=255, width=1)
btn.paste(Image.new("RGBA", (W, H), (0, 0, 0, 255)), (0, 0), frame)
btn.save("playdate/Source/images/bmc_button.png")
print("ok", n, W, H)
