"""Row-marker icons for lists: sub-menu chevron, checkbox off/on, radio
off/on. All 11 rows tall and 11 design pixels wide, so a marker never
changes the layout when it changes state.

Lowres: hand-drawn below ('#' = ink). Hires (22 x 11, tall pixels): drawn
from vector shapes on the hires grid and thresholded at 50% coverage, like
tools/hiicons.py, so curves and the tick use half-pixel steps.
"""
from PIL import Image, ImageDraw

SIZE = 11
ORDER = ["arrow", "check_off", "check_on", "radio_off", "radio_on"]   # MARK_ARROW.. MARK_RADIO_ON

LORES = {
    "arrow": [
        "...........",
        "..#........",
        "..##.......",
        "...##......",
        "....##.....",
        ".....##....",
        "....##.....",
        "...##......",
        "..##.......",
        "..#........",
        "...........",
    ],
    "check_off": [
        "###########",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "###########",
    ],
    "check_on": [
        "###########",
        "#.........#",
        "#.......#.#",
        "#......##.#",
        "#.#...##..#",
        "#.##.##...#",
        "#..###....#",
        "#...#.....#",
        "#.........#",
        "#.........#",
        "###########",
    ],
    "radio_off": [
        "...#####...",
        "..#.....#..",
        ".#.......#.",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        "#.........#",
        ".#.......#.",
        "..#.....#..",
        "...#####...",
    ],
    "radio_on": [
        "...#####...",
        "..#.....#..",
        ".#.......#.",
        "#...###...#",
        "#..#####..#",
        "#..#####..#",
        "#..#####..#",
        "#...###...#",
        ".#.......#.",
        "..#.....#..",
        "...#####...",
    ],
}

S = 16   # canvas units per design pixel


def _p(x, y):
    return (x * S, y * S)


def _hires_draw(name, d):
    if name == "arrow":
        d.line([_p(2.6, 1.4), _p(7.6, 5.5), _p(2.6, 9.6)], fill=255, width=int(1.5 * S), joint="curve")
        return
    if name.startswith("check"):
        d.rectangle([0, 0, SIZE * S - 1, SIZE * S - 1], fill=255)
        d.rectangle([S, S, (SIZE - 1) * S - 1, (SIZE - 1) * S - 1], fill=0)
        if name == "check_on":
            d.line([_p(2.4, 5.2), _p(4.4, 7.6), _p(8.6, 2.6)], fill=255, width=int(1.5 * S),
                   joint="curve")
        return
    c, r = SIZE / 2, SIZE / 2
    d.ellipse([_p(c - r, c - r), _p(c + r, c + r)], fill=255)
    d.ellipse([_p(c - r + 1.05, c - r + 1.05), _p(c + r - 1.05, c + r - 1.05)], fill=0)
    if name == "radio_on":
        d.ellipse([_p(c - 2.6, c - 2.6), _p(c + 2.6, c + 2.6)], fill=255)


def lores(name):
    rows = []
    for line in LORES[name]:
        assert len(line) == SIZE, (name, line)
        v = 0
        for x, ch in enumerate(line):
            if ch == "#":
                v |= 0x8000 >> x
        rows.append(v)
    return rows


def hires(name):
    big = Image.new("L", (SIZE * S, SIZE * S), 0)
    _hires_draw(name, ImageDraw.Draw(big))
    small = big.resize((SIZE * 2, SIZE), Image.BOX)
    rows = []
    for y in range(SIZE):
        v = 0
        for x in range(SIZE * 2):
            if small.getpixel((x, y)) >= 128:
                v |= 1 << (31 - x)
        rows.append(v)
    return rows
