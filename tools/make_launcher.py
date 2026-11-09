"""Playdate launcher art for SFTV RX (playdate/Source/launcher, pdxinfo imagePath=launcher):
card.png 350x155, card-highlighted/ (loop), card-pressed.png, icon.png / icon-pressed.png 32x32,
launchImage.png 400x240 (= the app's first screen) and launchImages/ (card -> first screen at
20 fps). Built from the app's own pieces: the dolphin, the console, the Roobert fonts.
Usage: python3 tools/make_launcher.py   (from the repo root; needs Pillow)"""
import math, os
from PIL import Image, ImageDraw

SRC = "playdate/Source"
OUT = SRC + "/launcher"
WHITE, BLACK = 255, 0


# ---- Playdate .fnt (text format + glyph table image) ----
class Font:
    def __init__(self, base):
        fnt = open(f"{SRC}/fonts/{base}.fnt", encoding="utf-8").read().splitlines()
        tbl = [f for f in os.listdir(f"{SRC}/fonts") if f.startswith(base + "-table-")][0]
        cw, ch = map(int, tbl[:-4].split("-")[-2:])
        self.img = Image.open(f"{SRC}/fonts/{tbl}").convert("RGBA")
        self.cw, self.ch, self.h = cw, ch, ch
        self.tracking, self.w, self.idx = 0, {}, {}
        cols = self.img.width // cw
        i = 0
        for line in fnt:
            if line.startswith("tracking="):
                self.tracking = int(line.split("=")[1]); continue
            if not line or line.startswith("--") or "\t" not in line:
                continue
            c, w = line.split("\t")[0], line.split("\t")[-1]
            c = " " if c == "space" else c
            self.w[c] = int(w); self.idx[c] = i; i += 1
        self.cols = cols

    def width(self, s):
        return sum(self.w.get(c, 0) for c in s) + self.tracking * max(0, len(s) - 1)

    def draw(self, im, x, y, s, color=BLACK):
        for c in s:
            if c in self.idx:
                k = self.idx[c]
                g = self.img.crop(((k % self.cols) * self.cw, (k // self.cols) * self.ch, (k % self.cols + 1) * self.cw, (k // self.cols + 1) * self.ch))
                a = g.getchannel("A").point(lambda v: 255 if v > 127 else 0)
                dark = g.convert("L").point(lambda v: 255 if v < 128 else 0)
                m = Image.composite(dark, Image.new("L", g.size, 0), a)
                im.paste(color, (x, y), m)
            x += self.w.get(c, 0) + self.tracking


big = Font("Roobert-20-Medium")
ui = Font("Roobert-10-Bold")


def load1(name):
    return Image.open(f"{SRC}/images/{name}.png").convert("L")


def scale2(im):
    return im.resize((im.width * 2, im.height * 2), Image.NEAREST)


def fs_dither(gray):  # Floyd-Steinberg to 1 bit, gray 0..255 (255 white)
    w, h = gray.size
    px = [[float(gray.getpixel((x, y))) for x in range(w)] for y in range(h)]
    out = Image.new("L", (w, h), WHITE)
    for y in range(h):
        for x in range(w):
            o = px[y][x]; n = 255.0 if o >= 128 else 0.0
            out.putpixel((x, y), int(n)); e = o - n
            for dx, dy, k in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                if 0 <= x + dx < w and y + dy < h:
                    px[y + dy][x + dx] += e * k / 16
    return out


COUCH = 8  # frames of the Flipper desktop animation L1_Tv (128x47): the dolphin on the couch


def screen_picture(w, h, frame=0):
    """What the console shows: the dolphin on the couch with its bowl, cut 1:1 from the right
    of the 128x47 frame (shrunk, the line art turned to mush; the TV on the left does not fit,
    the console is the TV)."""
    f = Image.open(f"tools/assets/couch/{frame % COUCH}.png").convert("L")
    x0, y0 = 128 - w, 0
    return f.crop((x0, y0, x0 + w, y0 + h))


def title(im, x, y):
    """RX pill + "Slow Flipper TV" as on the app's main menu; returns the right edge."""
    d = ImageDraw.Draw(im)
    cap_top, cap_h = y + 4, 20
    pill_w = ui.width("RX") + 14
    d.rounded_rectangle([x, cap_top, x + pill_w - 1, cap_top + cap_h - 1], cap_h // 2, fill=BLACK)
    ui.draw(im, x + 7, cap_top + (cap_h - 10) // 2 - 2, "RX", WHITE)
    big.draw(im, x + pill_w + 8, y, "Slow Flipper TV")
    return x + pill_w + 8 + big.width("Slow Flipper TV")


CON_SX, CON_SY, CON_SW, CON_SH = 7, 7, 88, 53  # console screen interior
DROPS = 7  # drops on the spout arc


def scene(t=0.0, reveal=1.0, mouth=False, frame=0, W=350, H=155):
    """The cover: title on top, the dolphin spouting into the console showing a picture.
    t: spout phase 0..1; reveal: share of the picture drawn (top down)."""
    im = Image.new("L", (W, H), WHITE)
    tw = 39 + 8 + big.width("Slow Flipper TV")
    title(im, (W - tw) // 2, 6)
    dol = scale2(load1("dolphin_open" if mouth else "dolphin"))
    dx, dy = 22, H - dol.height - 8
    im.paste(dol, (dx, dy))
    con = load1("console")
    cx, cy = W - con.width - 22, H - con.height + 4  # the console's bottom edge runs off the card
    im.paste(con, (cx, cy))
    pic = screen_picture(CON_SW - 6, CON_SH - 6, frame)
    rows = int(round(pic.height * reveal))
    if rows > 0:
        im.paste(pic.crop((0, 0, pic.width, rows)), (cx + CON_SX + 3, cy + CON_SY + 3))
    if 0 < rows < pic.height:  # the drawing line
        ImageDraw.Draw(im).line([(cx + CON_SX + 3, cy + CON_SY + 3 + rows), (cx + CON_SX + 3 + pic.width - 1, cy + CON_SY + 3 + rows)], fill=BLACK)
    # spout: drops on an arc from the blowhole to the screen (as draw_spout in the app)
    d = ImageDraw.Draw(im)
    x0, y0 = dx + 70, dy + 2                      # the blowhole (as in the app's scene)
    x1, y1 = cx + CON_SX + 4, cy + CON_SY + 8     # the screen's top-left corner
    for k in range(DROPS):
        u = (t + k / DROPS) % 1.0
        x = x0 + (x1 - x0) * u
        y = y0 + (y1 - y0) * u - 14 * math.sin(math.pi * u)  # low: the title is above
        r = 1.5 + 2 * math.sin(math.pi * u)
        d.ellipse([x - r, y - r, x + r, y + r], fill=BLACK)
    return im


def ease(t):  # cubic ease-in-out
    return 4 * t * t * t if t < 0.5 else 1 - (2 - 2 * t) ** 3 / 2


B4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def icon(pressed):
    """32x32: the console from the card (crank, d-pad, buttons) with a sphere on its screen;
    inverted while pressed."""
    im = Image.new("L", (32, 32), WHITE)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([3, 2, 25, 30], 3, fill=BLACK)           # body
    d.rectangle([26, 9, 27, 13], fill=BLACK)                      # crank arm
    d.rectangle([26, 9, 29, 10], fill=BLACK)                      # crank knob
    d.rectangle([6, 5, 22, 17], fill=WHITE)                       # screen
    d.ellipse([10, 6, 19, 15], fill=BLACK)                        # the sphere, lit top left
    d.rectangle([12, 8, 13, 9], fill=WHITE)
    d.line([(6, 24), (12, 24)], fill=WHITE); d.line([(9, 21), (9, 27)], fill=WHITE)      # d-pad
    d.rectangle([16, 24, 17, 25], fill=WHITE); d.rectangle([20, 22, 21, 23], fill=WHITE)  # A, B
    if pressed:
        im = im.point(lambda v: 255 - v)
    return im


def card_origin_for_menu():
    """Where the card must sit so its title lands on the main menu's title (x 16, y 8)."""
    tw = 39 + 8 + big.width("Slow Flipper TV")
    return 16 - (350 - tw) // 2, 8 - 6


if __name__ == "__main__":
    os.makedirs(OUT + "/card-highlighted", exist_ok=True)
    os.makedirs(OUT + "/launchImages", exist_ok=True)
    card = scene()
    card.save(f"{OUT}/card.png")
    # highlighted: intro = the first couch frame draws itself top down (each step held 3
    # launcher frames); loop = the couch scene (each of its frames held HOLD launcher frames)
    # while the drops flow from the dolphin into the console. The drops move 1/(DROPS*12) of the
    # arc per launcher frame (a whole spacing per frame looked still); over the loop that adds
    # up to whole spacings, so the loop has no seam.
    intro, HOLD = 12, 8
    loop = COUCH * HOLD
    step = 1.0 / (DROPS * 12)
    for i in range(intro):
        scene(t=i * 3 * step, reveal=(i + 1) / (intro + 1)).save(f"{OUT}/card-highlighted/{i + 1}.png")
    t0 = intro * 3 * step
    for i in range(loop):
        scene(t=t0 + i * step, frame=i // HOLD).save(f"{OUT}/card-highlighted/{intro + i + 1}.png")
    with open(f"{OUT}/card-highlighted/animation.txt", "w") as f:
        f.write("introFrames = " + ", ".join(f"{i + 1}x3" for i in range(intro)) + "\n")
        f.write("frames = " + ", ".join(str(intro + i + 1) for i in range(loop)) + "\n")
    pressed = Image.new("L", card.size, WHITE)
    pressed.paste(scene(t=t0), (0, 1))
    pressed.save(f"{OUT}/card-pressed.png")
    icon(False).save(f"{OUT}/icon.png")
    icon(True).save(f"{OUT}/icon-pressed.png")
    menu = Image.open("tools/assets/launch_menu.png").convert("L")  # the app's first screen
    menu.save(f"{OUT}/launchImage.png")
    # launchImages: the launcher starts from the card at (25, 43) over its own background.
    # 1..6: the card slides (eased) until its title sits on the menu's title, white spreading
    # from the card to the whole screen; 7..12: a Bayer dissolve into the menu.
    (ox, oy), slide, fade, n = card_origin_for_menu(), 6, 6, 0
    for i in range(1, slide + 1):
        e = ease(i / slide)
        x, y = round(25 + (ox - 25) * e), round(43 + (oy - 43) * e)
        fr = Image.new("LA", (400, 240), (WHITE, 0))
        l, t, r, b = round(x - x * e), round(y - y * e), round(x + 350 + (400 - x - 350) * e), round(y + 155 + (240 - y - 155) * e)
        fr.paste((WHITE, 255), (min(l, x), min(t, y), max(r, x + 350), max(b, y + 155)))
        fr.paste(Image.merge("LA", (card, Image.new("L", card.size, 255))), (x, y))
        n += 1; fr.save(f"{OUT}/launchImages/{n}.png")
    base = Image.new("L", (400, 240), WHITE); base.paste(card, (ox, oy))
    for i in range(1, fade + 1):
        lvl = i * 16 / fade
        fr = base.copy()
        mp, bp = menu.load(), fr.load()
        for yy in range(240):
            for xx in range(400):
                if B4[yy % 4][xx % 4] < lvl:
                    bp[xx, yy] = mp[xx, yy]
        n += 1; fr.save(f"{OUT}/launchImages/{n}.png")
    print("launcher ok:", n, "launch frames")
