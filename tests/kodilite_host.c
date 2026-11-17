/* Host preview for the Kodi lite layout (src/layouts/kodilite.c): its
 * screens drawn by the drawing kit (lkcore.c) into a picture in memory
 * that keeps only what a 16, 24 or 32-bit screen keeps, with the smooth
 * fonts' own glyphs and icons (assets/Fonts/amb-smooth/<lines>) and
 * made-up models like the mockups'. Written as
 * build/kodilite-<screen>-<w>x<h>.ppm; checked only for drawing outside
 * the screen or nothing drawn. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/layout.h"

extern const struct LayoutEngine layout_engine;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* ---------------------------------------------------------------- the screen */
static int SW, SH, BITS;
static unsigned long *px;
static unsigned long scratch[2048 * 16];
static struct LkOut out;
static long drawn_px;

static unsigned long keep(unsigned long c)
{
    unsigned long r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    if (BITS >= 24)
        return c & 0xFFFFFF;
    r = (r >> 3) << 3 | r >> 5;
    b = (b >> 3) << 3 | b >> 5;
    g = BITS == 16 ? ((g >> 2) << 2 | g >> 6) : ((g >> 3) << 3 | g >> 5);
    return r << 16 | g << 8 | b;
}

static void s_fill(void *ctx, long x, long y, long w, long h, unsigned long rgb)
{
    long i, j;
    (void)ctx;
    if (x < 0 || y < 0 || x + w > SW || y + h > SH) {
        printf("FAIL fill outside the screen: %ld,%ld %ldx%ld\n", x, y, w, h);
        fails++;
        return;
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            px[(y + j) * SW + x + i] = keep(rgb);
    drawn_px += w * h;
}

static void s_rows(void *ctx, long x, long y, long w, long h, const unsigned long *p, long mod)
{
    long i, j;
    (void)ctx;
    if (x < 0 || y < 0 || x + w > SW || y + h > SH) {
        printf("FAIL rows outside the screen: %ld,%ld %ldx%ld\n", x, y, w, h);
        fails++;
        return;
    }
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            px[(y + j) * SW + x + i] = keep(p[j * mod + i]);
    drawn_px += w * h;
}

/* ---------------------------------------------------------------- the smooth font */
struct Glyph { unsigned adv16; int xo, yo; unsigned w, h; unsigned long off; };
struct Face { unsigned lo, hi; struct Glyph *gl; const unsigned char *blob; };
static struct {
    unsigned char *data;
    unsigned cell_h, base, nf;
    struct Face face[4];
    unsigned icon_n, icon_cnt;
    const unsigned char *icons;
} F;

static unsigned be16(const unsigned char *p) { return p[0] << 8 | p[1]; }
static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | p[2] << 8 | p[3];
}

static int font_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n, p = 4;
    unsigned i, k;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(F.data);
    for (i = 0; i < 4; i++)
        free(F.face[i].gl);
    memset(&F, 0, sizeof(F));
    F.data = malloc(n);
    if (fread(F.data, 1, n, f) != (size_t)n || memcmp(F.data, "AMSM", 4)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    F.cell_h = be16(F.data + p + 4);
    F.base = be16(F.data + p + 6);
    F.nf = be16(F.data + p + 8);
    p += 10;
    for (k = 0; k < F.nf && k < 4; k++) {
        struct Face *fc = &F.face[k];
        fc->lo = be16(F.data + p);
        fc->hi = be16(F.data + p + 2);
        p += 4;
        fc->gl = calloc(fc->hi - fc->lo + 1, sizeof(struct Glyph));
        for (i = 0; i <= fc->hi - fc->lo; i++, p += 10) {
            fc->gl[i].adv16 = be16(F.data + p);
            fc->gl[i].xo = (signed char)F.data[p + 2];
            fc->gl[i].yo = (signed char)F.data[p + 3];
            fc->gl[i].w = F.data[p + 4];
            fc->gl[i].h = F.data[p + 5];
            fc->gl[i].off = be32(F.data + p + 6);
        }
        n = be32(F.data + p);
        p += 4;
        fc->blob = F.data + p;
        p += n;
    }
    F.icon_n = be16(F.data + p);
    F.icon_cnt = be16(F.data + p + 2);
    F.icons = F.data + p + 4;
    return 1;
}

static int nib(const unsigned char *d, unsigned long n)
{
    return n & 1 ? d[n >> 1] & 15 : d[n >> 1] >> 4;
}

static const struct Glyph *glyph(int face, unsigned char c, const struct Face **fc)
{
    *fc = &F.face[face < (int)F.nf ? face : 0];
    if (c < (*fc)->lo || c > (*fc)->hi)
        c = '?';
    return &(*fc)->gl[c - (*fc)->lo];
}

static WORD h_textlen(const char *s, LONG n, int face)
{
    const struct Face *fc;
    long pen = 0;
    LONG i;
    for (i = 0; i < n; i++)
        pen += glyph(face, (unsigned char)s[i], &fc)->adv16;
    return (WORD)((pen + 8) >> 4);
}

static WORD h_fit(const char *s, LONG n, int face, WORD maxw)
{
    LONG k = 0;
    while (k < n && h_textlen(s, k + 1, face) <= maxw)
        k++;
    return (WORD)k;
}

static unsigned char cov[2048 * 64];

/* text as sm_text draws it: the pen in 1/16 px, each glyph from its
 * nearest quarter-pixel phase; coverage to lk_cover in its tight box */
static WORD h_text(WORD x, WORD y, const char *s, LONG n, int face, ULONG fg, const struct LkBg *bg,
                   WORD maxw)
{
    const struct Face *fc;
    long pen, minx = 1 << 20, maxx = -(1 << 20), miny = 1 << 20, maxy = -(1 << 20), bw, bh;
    LONG i;
    unsigned j, k;
    if (maxw > 0)
        n = h_fit(s, n, face, maxw);
    for (pen = 0, i = 0; i < n; i++) {
        const struct Glyph *g = glyph(face, (unsigned char)s[i], &fc);
        long q = (pen + 2) >> 2, gx = (q >> 2) + g->xo;
        if (g->w) {
            if (gx < minx) minx = gx;
            if (gx + (long)g->w > maxx) maxx = gx + g->w;
            if (g->yo < miny) miny = g->yo;
            if (g->yo + (long)g->h > maxy) maxy = g->yo + g->h;
        }
        pen += g->adv16;
    }
    if (maxx <= minx)
        return h_textlen(s, n, face);
    bw = maxx - minx;
    bh = maxy - miny;
    if (bw * bh > (long)sizeof(cov))
        return 0;
    memset(cov, 0, bw * bh);
    for (pen = 0, i = 0; i < n; i++) {
        const struct Glyph *g = glyph(face, (unsigned char)s[i], &fc);
        long q = (pen + 2) >> 2, gx = (q >> 2) + g->xo - minx;
        if (g->w) {
            const unsigned char *d = fc->blob + g->off + (q & 3) * ((g->w * g->h + 1) / 2);
            for (j = 0; j < g->h; j++)
                for (k = 0; k < g->w; k++) {
                    unsigned char v = nib(d, j * g->w + k), *c = &cov[(g->yo - miny + j) * bw + gx + k];
                    if (v > *c)
                        *c = v;
                }
        }
        pen += g->adv16;
    }
    lk_cover(&out, x + minx, y + miny, cov, bw, bh, bw, fg, bg);
    return h_textlen(s, n, face);
}

static WORD h_font_h(int face)
{
    (void)face;
    return (WORD)F.cell_h;
}

static void h_icon(int section, WORD x, WORD y, ULONG fg, const struct LkBg *bg)
{
    /* LSEC_* to the program's ICON_* (games, mods, demos, videos,
     * pictures, music, radio, settings, about) */
    static const int icon[] = { 0, 2, 5, 1, 4, 3, 6, 7, 8 };
    unsigned n = F.icon_n, i;
    int k = section >= 0 && section < 9 ? icon[section] : 8;
    const unsigned char *d = F.icons + k * ((n * n + 1) / 2);
    if ((unsigned)k >= F.icon_cnt)
        return;
    for (i = 0; i < n * n; i++)
        cov[i] = nib(d, i);
    lk_cover(&out, x, y, cov, n, n, n, fg, bg);
}

/* ---------------------------------------------------------------- the rest of the host */
static void h_clip(WORD x0, WORD y0, WORD x1, WORD y1) { lk_clip(&out, x0, y0, x1, y1); }
static void h_fill(WORD x, WORD y, WORD w, WORD h, ULONG c) { lk_fill(&out, x, y, w, h, c); }
static void h_vgrad(WORD x, WORD y, WORD w, WORD h, ULONG a, ULONG b) { lk_vgrad(&out, x, y, w, h, a, b); }
static void h_hgrad(WORD x, WORD y, WORD w, WORD h, ULONG a, ULONG b) { lk_hgrad(&out, x, y, w, h, a, b); }
static void h_rrect(WORD x, WORD y, WORD w, WORD h, WORD r, ULONG c, ULONG bg) { lk_rrect(&out, x, y, w, h, r, c, bg); }
static void h_frame(WORD x, WORD y, WORD w, WORD h, WORD t, ULONG c) { lk_frame(&out, x, y, w, h, t, c); }

static unsigned long imgbuf[1024 * 1024];
static ULONG imgpx[1024 * 1024];
static void h_image(const struct LImage *im, WORD x, WORD y)
{
    unsigned long i, n = (unsigned long)im->mod * im->h;
    for (i = 0; i < n; i++)
        imgbuf[i] = im->px[i];
    lk_image(&out, x, y, imgbuf, im->w, im->h, im->mod);
}

/* a made-up cover: a diagonal gradient with a band, its colours from the name */
static struct LImage pic;
static const struct LImage *h_picture(const char *file, UWORD w, UWORD h)
{
    unsigned long seed = 5381, a, b;
    unsigned x, y, s;
    const char *p;
    for (p = file; *p; p++)
        seed = seed * 33 + (unsigned char)*p;
    a = 0x400000 | (seed & 0x3F7F9F);
    b = 0x101010 | ((seed >> 8) & 0xD0A060);
    s = w < h ? w : h;
    if (strstr(file, "shot"))
        s = 0;                  /* a screenshot: 4:3 */
    pic.w = s ? s : w;
    pic.h = s ? s : (w * 3 / 4 <= h ? w * 3 / 4 : h);
    pic.mod = pic.w;
    for (y = 0; y < pic.h; y++)
        for (x = 0; x < pic.w; x++) {
            unsigned long t = (x + y) * 255 / (pic.w + pic.h), c = 0;
            int k;
            for (k = 0; k <= 16; k += 8) {
                long ca = (a >> k) & 255, cb = (b >> k) & 255;
                c |= (unsigned long)(ca + (cb - ca) * (long)t / 255) << k;
            }
            if (y > pic.h * 2u / 3 && y < pic.h * 2u / 3 + pic.h / 12u)
                c = 0xF0E6D2;
            imgpx[y * pic.w + x] = (ULONG)c;
        }
    pic.px = imgpx;
    return &pic;
}

/* ---- the models */
static const char *mods[] = { "Games", "Mods", "Demos", "Videos", "Pictures", "Music", "Radio",
                              "Settings", "About" };
static const UBYTE secs[] = { LSEC_GAMES, LSEC_MODS, LSEC_DEMOS, LSEC_VIDEOS, LSEC_PICTURES,
                              LSEC_MUSIC, LSEC_RADIO, LSEC_SETTINGS, LSEC_OTHER };
static struct LItem music_rows[] = {
    { "Artists", NULL, "142", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "Albums", NULL, "312", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "Songs", NULL, "3,904", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "Genres", NULL, "28", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "Recently added", NULL, NULL, NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "Now playing", NULL, "Amiga Dreams", NULL, LI_ACTION, LM_PLAYING, LIF_NOW, 0 },
    { "Music folders", NULL, "Work:Music", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "Plex", NULL, "Living room", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
};
static const struct LRow row = { "Music", 8, music_rows };

static struct LItem set_rows[] = {
    { "Screen", NULL, NULL, NULL, LI_OTHER, 0, LIF_HEAD, 0 },
    { "Screen mode", NULL, "800 x 600, 24-bit", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Style", NULL, "Smooth", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Double buffering", NULL, NULL, NULL, LI_SETTING, LM_CHECK_ON, 0, 0 },
    { "Colours", NULL, "Midnight", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Layout", NULL, "Kodi lite", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Visualisation", NULL, "Spectrum", NULL, LI_SETTING, LM_CYCLE, 0, 0 },
    { "Full screen visual (F)", NULL, "Flow", NULL, LI_SETTING, LM_CYCLE, 0, 0 },
    { "Jump scrolling (by letter)", NULL, NULL, NULL, LI_SETTING, LM_CHECK_OFF, 0, 0 },
    { "Slideshow transition", NULL, "Fade", NULL, LI_SETTING, LM_CYCLE, 0, 0 },
    { "Window", NULL, NULL, NULL, LI_OTHER, 0, LIF_HEAD, 0 },
    { "Text size", NULL, "Medium", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Open in a window", NULL, NULL, NULL, LI_SETTING, LM_CHECK_OFF, LIF_DISABLED, 0 },
};
static struct LItem game_rows[] = {
    { "Another World", NULL, "1991", NULL, LI_GAME, 0, 0, 0 },
    { "Banshee", NULL, "1994", NULL, LI_GAME, 0, 0, 0 },
    { "Cannon Fodder", NULL, "1993", NULL, LI_GAME, 0, 0, 0 },
    { "Flashback", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
    { "Lemmings", NULL, "1991", NULL, LI_GAME, 0, 0, 0 },
    { "Lotus Turbo Challenge 2", NULL, "1991", NULL, LI_GAME, 0, 0, 0 },
    { "Monkey Island 2", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
    { "Pinball Dreams", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
    { "Pinball Fantasies", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
    { "Project-X", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
    { "Sensible Soccer", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
    { "Shadow of the Beast", NULL, "1989", NULL, LI_GAME, 0, 0, 0 },
    { "Speedball 2", NULL, "1990", NULL, LI_GAME, 0, 0, 0 },
    { "Turrican II", NULL, "1991", NULL, LI_GAME, 0, 0, 0 },
    { "Worms", NULL, "1995", NULL, LI_GAME, 0, 0, 0 },
    { "Zool", NULL, "1992", NULL, LI_GAME, 0, 0, 0 },
};
static const char *game_facts[] = { "Pinball Fantasies", "Digital Illusions, 1992",
                                    "Genre: Pinball", "Players: 1-8", "Chipset: AGA, ECS",
                                    "Four tables: Partyland, Speed Devils, Billion Dollar Gameshow, Stones 'n' Bones." };
static const char *text_lines[] = { "Ambiance", "",
    "A media centre for the Amiga: games and demos from AGS, music, modules,",
    "pictures, videos, radio and Plex, on a TV with a joystick or the keys.",
    "", "Copyright (c) 2026 Brian King.", "",
    "The layouts draw the screen in a style of their own; the classic layout",
    "is Ambiance's own and always there (hold Left Shift as it starts)." };
static const char *next[] = { "Paula's Lament", "Copper Bars" };

static int scene;               /* which page model() gives */
static struct LItem *rows_of;
static const struct LItem *item(LONG i) { return &rows_of[i]; }

static const void *h_model(int screen)
{
    static struct LHome home;
    static struct LPage page;
    static struct LNow now;
    if (screen == LS_HOME) {
        memset(&home, 0, sizeof(home));
        home.n = 9;
        home.sel = 5;
        home.sections = secs;
        home.names = mods;
        home.nrows = 1;
        home.rows = &row;
        return &home;
    }
    if (screen == LS_NOW) {
        static UBYTE lev[4] = { 200, 150, 230, 120 };
        memset(&now, 0, sizeof(now));
        now.kind = LN_TRACK;
        now.title = "Amiga Dreams";
        now.artist = "The Paula Channels";
        now.album = "Copper & Chrome";
        now.year = "(1993)";
        now.format = "MP3, 44.1 kHz";
        now.source = "Music files";
        now.art = "cover-copper";
        now.pos_ms = 83000;
        now.len_ms = 252000;
        now.nnext = 2;
        now.next = next;
        now.nlevels = 4;
        now.levels = lev;
        return &now;
    }
    memset(&page, 0, sizeof(page));
    page.hint = "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back";
    if (scene == 0) {
        page.section = LSEC_SETTINGS;
        page.ncrumb = 2;
        page.crumb[0] = "Settings";
        page.crumb[1] = "Display";
        rows_of = set_rows;
        page.list.count = sizeof(set_rows) / sizeof(set_rows[0]);
        page.list.sel = 5;
        page.list.item = item;
    } else if (scene == 1) {
        page.section = LSEC_GAMES;
        page.ncrumb = 3;
        page.crumb[0] = "Games";
        page.crumb[1] = "All games";
        page.crumb[2] = "P";
        rows_of = game_rows;
        page.list.count = sizeof(game_rows) / sizeof(game_rows[0]);
        page.list.sel = 8;
        page.list.item = item;
        page.ndetails = 6;
        page.details = game_facts;
        page.picture = "shot-pinball";
    } else {
        page.section = LSEC_OTHER;
        page.ncrumb = 2;
        page.crumb[0] = "About";
        page.crumb[1] = "Ambiance";
        page.text = 1;
        page.ndetails = sizeof(text_lines) / sizeof(text_lines[0]);
        page.details = text_lines;
        page.message = "Press Esc again to quit Ambiance";
    }
    return &page;
}

/* what the layout asked for, as it asked */
static int acts[64][2], nacts;
static void h_act(int what, LONG arg)
{
    if (nacts < 64) {
        acts[nacts][0] = what;
        acts[nacts][1] = (int)arg;
        nacts++;
    }
}
static const char *h_tr(const char *s) { return s; }
static void h_log(const char *s) { printf("layout: %s\n", s); }
static ULONG clock_ms = 1000;
static ULONG h_now(void) { return clock_ms; }

static void save(const char *name)
{
    char path[200];
    FILE *f;
    long i;
    sprintf(path, "build/kodilite-%s-%dx%d.ppm", name, SW, SH);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    for (i = 0; i < (long)SW * SH; i++) {
        fputc((int)(px[i] >> 16) & 255, f);
        fputc((int)(px[i] >> 8) & 255, f);
        fputc((int)px[i] & 255, f);
    }
    fclose(f);
}

static void clicks(int w, int h);

static void run(int w, int h, int bits, const char *font)
{
    struct LayoutHost host;
    static const char *names[] = { "settings", "games", "text" };
    SW = w;
    SH = h;
    BITS = bits;
    free(px);
    px = calloc((size_t)w * h, sizeof(*px));
    if (!font_load(font)) {
        printf("FAIL no font %s\n", font);
        fails++;
        return;
    }
    memset(&out, 0, sizeof(out));
    out.fill = s_fill;
    out.rows = s_rows;
    out.bits = bits;
    out.buf = scratch;
    out.bufn = sizeof(scratch) / sizeof(scratch[0]);
    lk_clip(&out, 0, 0, w, h);
    memset(&host, 0, sizeof(host));
    host.w = w;
    host.h = h;
    host.bits = bits;
    host.tier = 3;
    host.clip = h_clip;
    host.fill = h_fill;
    host.vgrad = h_vgrad;
    host.hgrad = h_hgrad;
    host.rrect = h_rrect;
    host.frame = h_frame;
    host.image = h_image;
    host.text = h_text;
    host.textlen = h_textlen;
    host.fit = h_fit;
    host.font_h = h_font_h;
    host.icon = h_icon;
    host.icon_px = F.icon_n;
    host.picture = h_picture;
    host.model = h_model;
    host.act = h_act;
    host.tr = h_tr;
    host.log = h_log;
    host.now_ms = h_now;
    host.clock = "21:47";
    CHECK(layout_engine.open(&host));
    drawn_px = 0;
    layout_engine.draw(LS_HOME);
    CHECK(drawn_px >= (long)w * h);
    save("home");
    for (scene = 0; scene < 3; scene++) {
        drawn_px = 0;
        layout_engine.draw(LS_PAGE);
        CHECK(drawn_px >= (long)w * h);
        save(names[scene]);
    }
    drawn_px = 0;
    layout_engine.draw(LS_NOW);
    CHECK(drawn_px >= (long)w * h);
    layout_engine.tick(5000);
    save("now");
    CHECK(layout_engine.input(LS_HOME, LA_DOWN, 0) == LR_UNUSED);
    clicks(w, h);
    layout_engine.close();
}

/* the first act a click at x, y asks for (-1: none), a second apart */
static int click_at(int screen, int x, int y, int *arg)
{
    nacts = 0;
    clock_ms += 1000;
    layout_engine.pointer(screen, (WORD)x, (WORD)y, TRUE);
    if (arg)
        *arg = nacts ? acts[0][1] : -1;
    return nacts ? acts[0][0] : -1;
}

/* Clicks: Home's sections go in, a setting opens, a game is selected and
 * played with a double-click, a crumb goes back, the bars pause, what
 * plays opens Now playing */
static void clicks(int w, int h)
{
    int y, x, a, last = -1, seen = 0, ok;
    layout_engine.draw(LS_HOME);
    for (y = 0; y < h; y++)                 /* the sections, top to bottom */
        if (click_at(LS_HOME, 100 * w / 800, y, &a) == LAC_SECTION && a != last) {
            CHECK(a == last + 1 && nacts == 2 && acts[1][0] == LAC_ENTER);
            last = a;
            seen++;
        }
    CHECK(seen == 9);
    for (ok = 0, x = w / 2; x < w && !ok; x++)
        ok = click_at(LS_HOME, x, 16 * h / 600 + 6, NULL) == LAC_NOWPLAYING;
    CHECK(ok);
    scene = 0;                              /* Settings > Display: a row opens */
    layout_engine.draw(LS_PAGE);
    for (ok = 0, y = 0; y < h && !ok; y++)
        ok = click_at(LS_PAGE, 500 * w / 800, y, &a) == LAC_OPEN;
    CHECK(ok);
    scene = 1;                              /* a game: selected, then played */
    layout_engine.draw(LS_PAGE);
    for (ok = 0, y = 0; y < h && !ok; y++)
        if (click_at(LS_PAGE, 500 * w / 800, y, &a) == LAC_SELECT && a == 8) {
            clock_ms += 200;                /* a double-click */
            nacts = 0;
            layout_engine.pointer(LS_PAGE, (WORD)(500 * w / 800), (WORD)y, TRUE);
            ok = nacts == 1 && acts[0][0] == LAC_OPEN && acts[0][1] == 8;
        }
    CHECK(ok);
    for (ok = 0, y = 0; y < h && !ok; y++) /* the crumb "All games": back once */
        if (click_at(LS_PAGE, 120 * w / 800, y, NULL) == LAC_BACK)
            ok = nacts == 1;
    CHECK(ok);
    layout_engine.draw(LS_NOW);
    CHECK(click_at(LS_NOW, w / 2, h / 2, NULL) == LAC_PAUSE);
    CHECK(click_at(LS_NOW, 2, h - 2, NULL) == -1);
}

int main(void)
{
    run(800, 600, 24, "assets/Fonts/amb-smooth/600");
    run(640, 480, 16, "assets/Fonts/amb-smooth/480");
    run(1024, 768, 32, "assets/Fonts/amb-smooth/768");
    if (fails) {
        printf("%d failed\n", fails);
        return 1;
    }
    printf("Kodi lite drawn: 5 screens at 800x600/24, 640x480/16, 1024x768/32; clicks\n");
    return 0;
}
