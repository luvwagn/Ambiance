"""Reader for the smooth-style files written by tools/mksmooth.py, used by
the preview tool and to check the files. Draws text, icons and markers the
way AmigaMCE does: coverage quantised to LEVELS steps, each step blended
between the colour and the background."""
import struct

LEVELS = 8          # AmigaMCE draws 8 steps: 0 (none) .. 7 (full colour)


def _unpack(blob, off, n):
    out = []
    for i in range(n):
        b = blob[off + (i >> 1)]
        out.append(b >> 4 if not (i & 1) else b & 15)
    return out


class Smooth:
    def __init__(self, data):
        assert data[:4] == b"AMSM"
        p = 4
        ver, self.k256, self.cell_h, self.base, nf = struct.unpack_from(">HHHHH", data, p)
        p += 10
        self.faces = []
        for _ in range(nf):
            lo, hi = struct.unpack_from(">HH", data, p)
            p += 4
            gl = []
            for _c in range(lo, hi + 1):
                gl.append(struct.unpack_from(">HbbBBI", data, p))     # adv in 1/16 px
                p += 10
            n = struct.unpack_from(">I", data, p)[0]
            p += 4
            self.faces.append((lo, hi, gl, data[p:p + n]))
            p += n
        self.icon_n, cnt = struct.unpack_from(">HH", data, p)
        p += 4
        sz = (self.icon_n * self.icon_n + 1) // 2
        self.icons = [_unpack(data, p + i * sz, self.icon_n ** 2) for i in range(cnt)]
        p += cnt * sz
        self.mark_n, cnt = struct.unpack_from(">HH", data, p)
        p += 4
        sz = (self.mark_n * self.mark_n + 1) // 2
        self.marks = [_unpack(data, p + i * sz, self.mark_n ** 2) for i in range(cnt)]
        p += cnt * sz
        self.logo_w, self.logo_h, nc = struct.unpack_from(">HHH", data, p)
        p += 6
        self.logo_pal = [tuple(data[p + 3 * i:p + 3 * i + 3]) for i in range(nc)]
        p += 3 * nc
        self.logo_idx = data[p:p + self.logo_w * self.logo_h]

    def glyph(self, face, c):
        lo, hi, gl, blob = self.faces[face]
        if not lo <= c <= hi:
            c = ord("?")
        return gl[c - lo], blob

    def textlen(self, s, face=0):
        return (sum(self.glyph(face, c)[0][0] for c in s.encode("latin-1", "replace")) + 8) >> 4

    @staticmethod
    def level(v):
        return (v * (LEVELS - 1) + 7) // 15

    def _plot(self, img, x, y, lv, fg, bg):
        if lv and 0 <= x < img.width and 0 <= y < img.height:
            t = lv / (LEVELS - 1)
            img.putpixel((x, y), tuple(round(bg[i] + (fg[i] - bg[i]) * t) for i in range(3)))

    def text(self, img, x, y, s, fg, bg, face=0):
        """As sm_text: pen in 1/16 px, each glyph from its nearest quarter-pixel phase."""
        pen16 = 0
        for c in s.encode("latin-1", "replace"):
            (adv16, xo, yo, w, h, off), blob = self.glyph(face, c)
            q = (pen16 + 2) >> 2
            if w:
                vals = _unpack(blob, off + (q & 3) * ((w * h + 1) // 2), w * h)
                gx = x + (q >> 2) + xo
                for j in range(h):
                    for i in range(w):
                        self._plot(img, gx + i, y + yo + j, self.level(vals[j * w + i]), fg, bg)
            pen16 += adv16
        return x + ((pen16 + 8) >> 4)

    def icon(self, img, i, x, y, fg, bg):
        n = self.icon_n
        for j in range(n):
            for k in range(n):
                self._plot(img, x + k, y + j, self.level(self.icons[i][j * n + k]), fg, bg)

    def mark(self, img, i, x, y, fg, bg):
        n = self.mark_n
        for j in range(n):
            for k in range(n):
                self._plot(img, x + k, y + j, self.level(self.marks[i][j * n + k]), fg, bg)

    def logo(self, img, x, y):
        for j in range(self.logo_h):
            for i in range(self.logo_w):
                v = self.logo_idx[j * self.logo_w + i]
                if v != 255:
                    img.putpixel((x + i, y + j), self.logo_pal[v])
