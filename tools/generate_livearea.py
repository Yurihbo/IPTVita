#!/usr/bin/env python3
"""Generate Vita-safe PNG8 LiveArea art for the IPTVita VPK.

The art is original and derived from the IPTVita deep-blue/cyan concepts.
No external service, image download, or image generation is needed at build
time. Does NOT affect the media player or its buffering/decoding code.
Requires Pillow and DejaVu Sans (installed by the GitHub build workflow).
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "livearea"
OUT.mkdir(parents=True, exist_ok=True)
NAVY = (4, 15, 37, 255)
CYAN = (22, 227, 251, 255)
BLUE = (11, 122, 250, 255)
WHITE = (239, 250, 255, 255)
FONT_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_REGULAR = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONT_BOLD if bold else FONT_REGULAR, size)
    except OSError:
        return ImageFont.load_default()


def gradient(w, h, top=(3, 13, 35), bottom=(0, 56, 115)):
    im = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        # A little extra dark in the center to preserve text contrast.
        t = t * (0.90 + 0.10 * t)
        color = tuple(round(a * (1 - t) + b * t) for a, b in zip(top, bottom))
        d.line([(0, y), (w, y)], fill=(*color, 255))
    return im


def glow(base, shape, args, color, strength=10, width=3):
    """Soft cyan halo + crisp line without relying on any system SVG library."""
    halo = Image.new("RGBA", base.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(halo)
    if shape == "line":
        d.line(args, fill=(*color[:3], 220), width=width * 3, joint="curve")
    elif shape == "round":
        d.rounded_rectangle(args[:4], radius=args[4],
                            outline=(*color[:3], 180), width=width * 3)
    elif shape == "arc":
        d.arc(args[:4], args[4], args[5],
              fill=(*color[:3], 220), width=width * 3)
    blurred = halo.filter(ImageFilter.GaussianBlur(strength))
    base.alpha_composite(blurred)
    d2 = ImageDraw.Draw(base)
    if shape == "line":
        d2.line(args, fill=color, width=width, joint="curve")
    elif shape == "round":
        d2.rounded_rectangle(args[:4], radius=args[4], outline=color, width=width)
    elif shape == "arc":
        d2.arc(args[:4], args[4], args[5], fill=color, width=width)


def text(base, at, value, size, color=WHITE, bold=False, center=False):
    d = ImageDraw.Draw(base)
    f = font(size, bold)
    d.text(at, value, font=f, fill=color,
           anchor="mm" if center else "lt", stroke_width=0)


def tv_symbol(base, x, y, size):
    d = ImageDraw.Draw(base)
    glow(base, "round", (x, y, x + size * 1.5, y + size, size // 7),
         CYAN, strength=max(3, size // 8), width=max(2, size // 20))
    d = ImageDraw.Draw(base)
    cx = x + size * 0.75
    cy = y + size * 0.49
    r = size * 0.28
    d.polygon([(int(cx - r * .45), int(cy - r)),
               (int(cx + r * 1.10), int(cy)),
               (int(cx - r * .45), int(cy + r))], fill=WHITE)
    glow(base, "line", [(int(cx), int(y + size)), (int(cx), int(y + size * 1.13))],
         CYAN, strength=4, width=max(2, size // 22))
    glow(base, "line", [(int(cx-size*.24), int(y+size*1.13)),
                        (int(cx+size*.24), int(y+size*1.13))],
         CYAN, strength=3, width=max(2, size//25))
    # Broadcast arcs over the upper-right edge.
    for delta in (0, size // 5, size * 2 // 5):
        box = (int(x + size * 1.07-delta//2),
               int(y-size//4-delta//2),
               int(x+size*1.65+delta//2),
               int(y+size*.35+delta//2))
        glow(base, "arc", (*box, 270, 359),
             CYAN, strength=3, width=max(2, size//24))


def make_banner():
    w, h = 960, 544
    im = gradient(w, h)
    d = ImageDraw.Draw(im)
    rng = random.Random(221147)
    # Starfield and neon blue/cyan "streaming universe".
    for i in range(260):
        x = rng.randrange(w)
        y = rng.randrange(0, 445)
        rad = 1 if i % 4 else 2
        bright = (62, 175, 235, 175) if i % 3 else (166, 233, 255, 230)
        d.ellipse((x-rad, y-rad, x+rad, y+rad), fill=bright)
    # Curved planet horizon. This is background artwork, not a live TV frame.
    for k in range(8, 0, -1):
        glow_color = (8, int(114 + k*10), 255, 175)
        d.arc((-90, 425-k*3, 1050, 915+k*4), 188, 353,
              fill=glow_color, width=2)
    d.ellipse((-70, 463, 1030, 1030), fill=(4, 27, 72, 255))
    glow(im, "arc", (-90, 434, 1050, 928, 184, 355), CYAN, strength=20, width=6)
    # Orbiting light ribbons, inspired by the supplied streaming banner.
    glow(im, "arc", (-140, 72, 1120, 432, 5, 169), BLUE, strength=22, width=4)
    glow(im, "arc", (-140, 75, 1120, 435, 10, 168), CYAN, strength=12, width=3)
    # Peripheral media tiles communicate TV/live channels while staying
    # deliberately non-branded (no external copyrighted logos).
    for i in range(5):
        for side in (0, 1):
            x = 10 + i*23 if side == 0 else 760 + i*23
            y = 228 + (i%3)*87
            box = (x, y, x+155, y+70)
            d.rounded_rectangle(box, radius=11, fill=(8, 43, 96, 235),
                                outline=(11, 134, 210, 255), width=2)
            d.polygon([(x+63,y+19),(x+63,y+52),(x+92,y+35)],
                      fill=(75, 198, 247, 200))
    # Logo and brand.
    tv_symbol(im, 191, 91, 87)
    text(im, (354, 103), "IPT", 84, WHITE, True)
    text(im, (521, 103), "Vita", 84, CYAN, True)
    # Large purposeful static card: never imply a particular show is live.
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((219, 291, 741, 417), radius=25,
                        fill=(6, 24, 57, 255))
    glow(im, "round", (219, 291, 741, 417, 25), CYAN, strength=14, width=4)
    text(im, (480, 334), "SUA CENTRAL IPTV", 37, WHITE, True, True)
    text(im, (480, 380), "CANAIS  •  FAVORITOS  •  AO VIVO",
         18, CYAN, False, True)
    return im.convert("RGB")


def make_icon():
    w = h = 256
    im = gradient(w, h, (1, 14, 40), (0, 83, 169))
    d = ImageDraw.Draw(im)
    glow(im, "arc", (-80, -92, 333, 338, 21, 95),
         BLUE, strength=18, width=9)
    glow(im, "arc", (-105, 162, 350, 320, 187, 349),
         CYAN, strength=16, width=5)
    tv_symbol(im, 44, 38, 111)
    text(im, (128, 215), "IPTVita", 35, WHITE, True, True)
    return im.convert("RGB")


def make_startup():
    w, h = 280, 158
    im = gradient(w, h, (1, 24, 55), (3, 75, 146))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((4, 5, 275, 152), radius=17,
                        fill=(5, 25, 60, 255))
    glow(im, "round", (4, 5, 275, 152, 17), CYAN, strength=4, width=2)
    tv_symbol(im, 31, 39, 48)
    text(im, (116, 45), "IPTVita", 28, WHITE, True)
    text(im, (116, 91), "INICIAR", 19, CYAN, True)
    return im.convert("RGB")


def indexed_png(image, destination, size, limit):
    image = image.resize(size, Image.Resampling.LANCZOS)
    # Indexed non-alpha PNG is required by the Vita shell. Try progressively
    # fewer colors if necessary to stay under the strict VPK asset limits.
    for colors in (96, 64, 48, 32, 24, 16):
        converted = image.quantize(colors=colors,
                                   method=Image.Quantize.MEDIANCUT)
        converted.save(destination, "PNG", optimize=True, compress_level=9)
        if destination.stat().st_size <= limit:
            print(f"{destination.relative_to(ROOT)}: {size[0]}x{size[1]}, "
                  f"PNG8 {colors} colors, {destination.stat().st_size} bytes")
            return
    raise RuntimeError(f"Vita image exceeds asset size limit: {destination}")


def main():
    icon = make_icon()
    banner = make_banner()
    indexed_png(icon, OUT / "icon0.png", (128, 128), 128 * 1024)
    indexed_png(banner, OUT / "pic0.png", (960, 544), 256 * 1024)
    indexed_png(banner, OUT / "bg0.png", (840, 500), 128 * 1024)
    indexed_png(make_startup(), OUT / "startup.png",
                (280, 158), 64 * 1024)


if __name__ == "__main__":
    main()
