"""Hires 32x16 menu icons drawn from vector shapes (coordinates in lowres
pixels, 16x16 box), thresholded at 50% coverage onto the hires grid."""
import math
from PIL import Image, ImageDraw

S = 16          # canvas units per lowres pixel


def P(x, y):
    return (x * S, y * S)


def box(d, x0, y0, x1, y1, fill=255):
    d.rectangle([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], fill=fill)


def ring(d, cx, cy, r, w=1.0, fill=255):
    d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=fill)
    d.ellipse([(cx - r + w) * S, (cy - r + w) * S, (cx + r - w) * S, (cy + r - w) * S], fill=0)


def disc(d, cx, cy, r, fill=255):
    d.ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=fill)


def rrect_outline(d, x0, y0, x1, y1, r, w=1.0):
    d.rounded_rectangle([x0 * S, y0 * S, x1 * S - 1, y1 * S - 1], radius=r * S, fill=255)
    d.rounded_rectangle([(x0 + w) * S, (y0 + w) * S, (x1 - w) * S - 1, (y1 - w) * S - 1],
                        radius=max(0, r - w) * S, fill=0)


def line(d, x0, y0, x1, y1, w=1.0):
    d.line([P(x0, y0), P(x1, y1)], fill=255, width=int(w * S))


def games(d):
    rrect_outline(d, 1, 4, 15, 13, 4)
    box(d, 3.5, 8, 6.5, 9)            # d-pad, on whole rows
    box(d, 4.5, 7, 5.5, 10)
    box(d, 10, 7, 11.5, 8)            # buttons
    box(d, 11.5, 9, 13, 10)


def mods(d):
    rrect_outline(d, 2, 1, 14, 15, 1)
    # text lines on whole rows, words split by a one-hires-pixel-pair gap
    for y, words in ((3, ((4, 7), (8, 12))), (5, ((4, 9.5), (10.5, 12))), (7, ((4, 6.5), (7.5, 12))),
                     (9, ((4, 10), (11, 12))), (11, ((4, 7.5), (8.5, 10.5)))):
        for a, b in words:
            box(d, a, y, b, y + 1)


def demos(d):
    cx, cy, ro, ri = 8, 8.6, 7.6, 3.1
    pts = []
    for k in range(10):
        a = -math.pi / 2 + k * math.pi / 5
        r = ro if k % 2 == 0 else ri
        pts.append(P(cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=255)


def videos(d):
    rrect_outline(d, 0.5, 2, 15.5, 14, 0.5)
    for x in (2, 4.5, 7, 9.5, 12):
        box(d, x, 3, x + 1.5, 4)
        box(d, x, 12, x + 1.5, 13)
    box(d, 0.5, 4, 15.5, 5)
    box(d, 0.5, 11, 15.5, 12)
    d.polygon([P(6.5, 6), P(10.5, 8), P(6.5, 10)], fill=255)


def pictures(d):
    rrect_outline(d, 0.5, 2, 15.5, 14, 0.5)
    disc(d, 4.5, 5.8, 1.4)
    d.polygon([P(1.5, 13), P(6, 7.5), P(9, 10.5), P(11, 8.5), P(14.5, 13)], fill=255)


def music(d):
    d.ellipse([P(1.6, 10.6), P(6.2, 14.2)], fill=255)
    d.ellipse([P(8.6, 9.6), P(13.2, 13.2)], fill=255)
    box(d, 5.0, 2.5, 6.2, 12.5)
    box(d, 12.0, 1.5, 13.2, 11.5)
    d.polygon([P(5.0, 2.5), P(13.2, 1.0), P(13.2, 3.4), P(5.0, 4.9)], fill=255)


def radio(d):
    rrect_outline(d, 1, 5, 15, 14.5, 1.5)
    line(d, 3.5, 5.2, 12.5, 1.2, 1.1)
    ring(d, 11, 9.75, 2.4, 1.0)
    for y in (7.5, 9.5, 11.5):
        box(d, 3, y - 0.5, 7, y + 0.5)


def settings(d):
    cx, cy = 8, 8
    disc(d, cx, cy, 5.5)
    for k in range(8):
        a = k * math.pi / 4
        ca, sa = math.cos(a), math.sin(a)
        hw, r0, r1 = 1.55, 4.5, 7.4
        pts = [(cx + r0 * ca - hw * sa, cy + r0 * sa + hw * ca), (cx + r1 * ca - hw * sa, cy + r1 * sa + hw * ca),
               (cx + r1 * ca + hw * sa, cy + r1 * sa - hw * ca), (cx + r0 * ca + hw * sa, cy + r0 * sa - hw * ca)]
        d.polygon([P(*q) for q in pts], fill=255)
    disc(d, cx, cy, 2.4, fill=0)


def about(d):
    ring(d, 8, 8, 7.0, 1.05)
    disc(d, 8, 4.6, 1.15)
    box(d, 7.25, 7, 8.75, 12)
    box(d, 6.0, 7, 8.75, 8)
    box(d, 6.0, 11, 10.0, 12)


ICONS = [games, mods, demos, videos, pictures, music, radio, settings, about]


def render(fn):
    big = Image.new("L", (16 * S, 16 * S), 0)
    fn(ImageDraw.Draw(big))
    small = big.resize((32, 16), Image.BOX)
    rows = []
    for y in range(16):
        v = 0
        for x in range(32):
            if small.getpixel((x, y)) >= 128:
                v |= 1 << (31 - x)
        rows.append(v)
    return rows
