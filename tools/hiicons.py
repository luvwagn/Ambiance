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
    # two beamed quavers: a tune
    d.ellipse([P(1.2, 10.2), P(6.4, 14.2)], fill=255)
    d.ellipse([P(8.8, 9.2), P(14.0, 13.2)], fill=255)
    box(d, 5.2, 2.8, 6.4, 12.4)
    box(d, 12.8, 1.8, 14.0, 11.4)
    d.polygon([P(5.2, 2.8), P(14.0, 1.0), P(14.0, 3.8), P(5.2, 5.6)], fill=255)


def demos(d):
    # a wireframe cube, the demo effect of them all
    f, b = (1.5, 5.5, 10.5, 14.5), (5.5, 1.5, 14.5, 10.5)
    for x0, y0, x1, y1 in (f, b):
        d.rectangle([P(x0, y0), P(x1, y1)], outline=255, width=int(1.0 * S))
    for (ax, ay), (bx, by) in (((f[0], f[1]), (b[0], b[1])), ((f[2], f[1]), (b[2], b[1])),
                               ((f[0], f[3]), (b[0], b[3])), ((f[2], f[3]), (b[2], b[3]))):
        line(d, ax, ay, bx, by, 1.0)


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
    # headphones: the band, and an ear cup each side
    d.arc([P(1.6, 2.2), P(14.4, 16.0)], 180, 360, fill=255, width=int(1.2 * S))
    d.rounded_rectangle([P(1.0, 8.6), P(4.8, 15.0)], radius=1.0 * S, fill=255)
    d.rounded_rectangle([P(11.2, 8.6), P(15.0, 15.0)], radius=1.0 * S, fill=255)


def radio(d):
    rrect_outline(d, 1, 5, 15, 14.5, 1.5)
    line(d, 3.5, 5.2, 12.5, 1.2, 1.1)
    ring(d, 11, 9.75, 2.4, 1.0)
    for y in (7.5, 9.5, 11.5):
        box(d, 3, y - 0.5, 7, y + 0.5)


def settings(d):
    # three mixer sliders, their knobs at different places
    for y, kx in ((3.5, 4.5), (7.5, 10.5), (11.5, 7.5)):
        box(d, 0.5, y - 0.5, 15.0, y + 0.5)
        d.rounded_rectangle([P(kx - 1.1, y - 1.7), P(kx + 1.1, y + 1.7)], radius=0.4 * S, fill=255)


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
