/* Host preview for the Showcase layout (src/layouts/showcase.c): its
 * screens drawn by the drawing kit (lkcore.c) into a picture in memory
 * that keeps only what a 16, 24 or 32-bit screen keeps, with the smooth
 * fonts' own glyphs and icons (assets/Fonts/amb-smooth/<lines>) and
 * made-up models like the mockups'. Written as
 * build/showcase-<screen>-<w>x<h>.ppm; checked only for drawing outside
 * the screen or nothing drawn. */
/* The layout drawn (Showcase unless built for another): its files'
 * prefix, and where its ways differ */
#ifndef LNAME
#define LNAME "showcase"
#define LTITLE "Showcase"
#endif
#ifdef HORIZON
#define WALL_RIGHT(a) ((a) > 7)                 /* (a column: the wall runs sideways) */
#define NOW_Y (h - 50 * h / 600)                /* (what plays: an inset at the bottom right) */
#define HOME_SOME 1                             /* (Home: the sections near the chosen one) */
#else
#define WALL_RIGHT(a) ((a) == 7)
#define NOW_Y (16 * h / 600 + 6)
#define HOME_SOME 0
#endif

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
static long drawn_px, copied_px;

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

static int s_copy(void *ctx, long x, long y, long w, long h, long dx, long dy)
{
    long j;
    (void)ctx;
    if (x < 0 || y < 0 || x + w > SW || y + h > SH || x + dx < 0 || y + dy < 0 || x + dx + w > SW ||
        y + dy + h > SH) {
        printf("FAIL copy outside the screen: %ld,%ld %ldx%ld by %ld,%ld\n", x, y, w, h, dx, dy);
        fails++;
        return 0;
    }
    if (dy > 0)
        for (j = h - 1; j >= 0; j--)
            memmove(&px[(y + j + dy) * SW + x + dx], &px[(y + j) * SW + x], (size_t)w * sizeof(*px));
    else
        for (j = 0; j < h; j++)
            memmove(&px[(y + j + dy) * SW + x + dx], &px[(y + j) * SW + x], (size_t)w * sizeof(*px));
    copied_px += w * h;
    return 1;
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
    unsigned cell_h, base, nf, title_h;
    struct Face face[4];                /* [3]: the title face (TITL), if the file has it */
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
    long n, p = 4, size;
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
    size = n;
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
    {
        /* the title face: "TITL" near the end (after the rest, Greek's too) */
        long q;
        for (q = size - 12; q > p; q--)
            if (!memcmp(F.data + q, "TITL", 4)) {
                struct Face *fc = &F.face[3];
                F.title_h = be16(F.data + q + 4);
                fc->lo = be16(F.data + q + 8);
                fc->hi = be16(F.data + q + 10);
                q += 12;
                fc->gl = calloc(fc->hi - fc->lo + 1, sizeof(struct Glyph));
                for (i = 0; i <= fc->hi - fc->lo; i++, q += 10) {
                    fc->gl[i].adv16 = be16(F.data + q);
                    fc->gl[i].xo = (signed char)F.data[q + 2];
                    fc->gl[i].yo = (signed char)F.data[q + 3];
                    fc->gl[i].w = F.data[q + 4];
                    fc->gl[i].h = F.data[q + 5];
                    fc->gl[i].off = be32(F.data + q + 6);
                }
                fc->blob = F.data + q + 4;
                break;
            }
    }
    return 1;
}

static int nib(const unsigned char *d, unsigned long n)
{
    return n & 1 ? d[n >> 1] & 15 : d[n >> 1] >> 4;
}

static const struct Glyph *glyph(int face, unsigned char c, const struct Face **fc)
{
    *fc = &F.face[face == LF_TITLE ? (F.title_h ? 3 : 1) : face < (int)F.nf ? face : 0];
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
    return (WORD)(face == LF_TITLE && F.title_h ? F.title_h : F.cell_h);
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
static BOOL h_copy(WORD x, WORD y, WORD w, WORD h, WORD dx, WORD dy)
{
    return lk_copy(&out, x, y, w, h, dx, dy) ? TRUE : FALSE;
}
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
    { "Recently added", NULL, NULL, "cover-recent", LI_FOLDER, LM_ARROW, 0, 0 },
    { "Now playing", NULL, "Amiga Dreams", NULL, LI_ACTION, LM_PLAYING, LIF_NOW, 0 },
    { "Music folders", NULL, "Work:Music", "cover-folders", LI_FOLDER, LM_ARROW, 0, 0 },
    { "Plex", NULL, "Living room", NULL, LI_FOLDER, LM_ARROW, 0, 0 },
    { "DF0_DVD", NULL, "DVD", NULL, LI_DISC, LM_NONE, 0, 0 },
};
static const struct LItem *music_item(LONG i) { return &music_rows[i]; }
static const struct LRow row = { "Music", 9, music_item };

static struct LItem set_rows[] = {
    { "Screen", NULL, NULL, NULL, LI_OTHER, 0, LIF_HEAD, 0 },
    { "Screen mode", NULL, "800 x 600, 24-bit", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Style", NULL, "Smooth", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Double buffering", NULL, NULL, NULL, LI_SETTING, LM_CHECK_ON, 0, 0 },
    { "Colours", NULL, "Midnight", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Layout", NULL, "Showcase", NULL, LI_SETTING, LM_ARROW, 0, 0 },
    { "Visualisation", NULL, "Spectrum", NULL, LI_SETTING, LM_CYCLE, 0, 0 },
    { "Full screen visual (F)", NULL, "Flow", NULL, LI_ACTION, LM_ARROW, 0, 0 },
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

static struct LItem album_rows[] = {
    { "Copper & Chrome", "The Paula Channels", "1993", "cover-copper", LI_ALBUM, LM_ARROW, LIF_NOW, 0 },
    { "Interference", "Sanity", "1994", "cover-interference", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Hardwired", "Crionics", "1991", "cover-hardwired", LI_ALBUM, LM_ARROW, 0, 0 },
    { "State of the Art", "Spaceballs", "1992", "cover-sota", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Enigma", "Phenomena", "1991", "cover-enigma", LI_ALBUM, LM_ARROW, 0, 0 },
    { "9 Fingers", "Spaceballs", "1993", "cover-9f", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Desert Dream", "Kefrens", "1993", "cover-dd", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Arte", "Sanity", "1993", NULL, LI_ALBUM, LM_ARROW, 0, 0 },
    { "Jesus on E's", "Lemon.", "1993", "cover-joe", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Hologramm", "Cascade", "1995", "cover-holo", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Elysium", "Sanity", "1991", "cover-elysium", LI_ALBUM, LM_ARROW, 0, 0 },
    { "Mental Hangover", "Scoopex", "1990", "cover-mh", LI_ALBUM, LM_ARROW, 0, 0 },
};
static struct LItem track_rows[] = {
    { "Paula's Lament", NULL, "4:12", NULL, LI_TRACK, LM_PLAYING, LIF_NOW, 0 },
    { "Copper Bars", NULL, "3:58", NULL, LI_TRACK, LM_PLAY, 0, 0 },
    { "Blitter Rain", NULL, "5:21", NULL, LI_TRACK, LM_PLAY, 0, 0 },
    { "Interleaved", NULL, "4:02", NULL, LI_TRACK, LM_PLAY, 0, 0 },
    { "Fast RAM", NULL, "3:33", NULL, LI_TRACK, LM_PLAY, 0, 0 },
    { "Agnus Rising", NULL, "6:10", NULL, LI_TRACK, LM_PLAY, 0, 0 },
};
static const char *album_facts[] = { "Genre", "Electronic, Chiptune", "Style", "Tracker" };
static const char *album_review = "Recorded on an A1200 with four Paula channels and nothing else, "
    "Copper & Chrome is the sound of a machine pushed to its edges.\nIts second side slows down: "
    "Agnus Rising takes six minutes to arrive, and is worth it.";
static const char *artist_bio = "The Paula Channels were formed in 1991 in Gothenburg by two "
    "demo-scene musicians who met at a copy party.\nThey released four albums on disk magazines "
    "before the first on vinyl.";
static const char *artist_facts[] = { "Formed", "1991", "Albums", "4" };

static int scene;               /* which page model() gives */
static struct LItem *rows_of;
static const struct LItem *item(LONG i) { return &rows_of[i]; }

static void crumbs(struct LPage *pg, const char *a, const char *b, const char *c)
{
    pg->section = LSEC_MUSIC;
    pg->ncrumb = 0;
    pg->crumb[pg->ncrumb++] = "Music";
    if (a)
        pg->crumb[pg->ncrumb++] = a;
    if (b)
        pg->crumb[pg->ncrumb++] = b;
    if (c)
        pg->crumb[pg->ncrumb++] = c;
    pg->hint = "Up/Down  move    Right/Fire  open    Return  play    Left/Esc  back";
}

static int frames_wall;
static ULONG text_frac;         /* the text page: how far through it */
static LONG wall_sel = 6;       /* the wall's chosen cover */
static LONG home_tile = -1;    /* Home's tiles in use: the one chosen */         /* the wall drawn as Videos' (4:3 frames) */
static const void *h_model(int screen)
{
    static struct LHome home;
    static struct LPage page;
    static struct LNow now;
    static struct LAlbum al;
    static struct LArtist ar;
    if (screen == LS_ALBUMS) {
        memset(&page, 0, sizeof(page));
        crumbs(&page, "Plex", "Albums", NULL);
        rows_of = album_rows;
        page.list.count = sizeof(album_rows) / sizeof(album_rows[0]);
        page.list.sel = wall_sel;
        page.list.item = item;
        page.frames = (UBYTE)frames_wall;
        if (frames_wall == 2) {         /* a wall of games' screenshots */
            static struct LItem gw[sizeof(game_rows) / sizeof(game_rows[0])];
            static const char *shots[] = { "shot-a", "shot-b", "shot-c", "shot-d" };
            unsigned k;
            for (k = 0; k < sizeof(gw) / sizeof(gw[0]); k++) {
                gw[k] = game_rows[k];
                gw[k].art = shots[k % 4];
            }
            memset(&page, 0, sizeof(page));
            crumbs(&page, "Games", "All games", NULL);
            rows_of = gw;
            page.list.count = sizeof(gw) / sizeof(gw[0]);
            page.list.sel = 8;
            page.list.item = item;
            page.frames = 1;
        }
        return &page;
    }
    if (screen == LS_ALBUM) {
        memset(&al, 0, sizeof(al));
        crumbs(&al.page, "Plex", "Albums", "Copper & Chrome");
        rows_of = track_rows;
        al.page.list.count = sizeof(track_rows) / sizeof(track_rows[0]);
        al.page.list.sel = 2;
        al.page.list.item = item;
        al.title = "Copper & Chrome";
        al.artist = "The Paula Channels";
        al.line = "1993 \xB7 Paula Records";
        al.rating = "8.5";
        al.art = "cover-copper";
        al.review = album_review;
        al.facts.n = 2;
        al.facts.facts = album_facts;
        return &al;
    }
    if (screen == LS_ARTIST) {
        memset(&ar, 0, sizeof(ar));
        crumbs(&ar.page, "Plex", "Artists", "The Paula Channels");
        rows_of = album_rows;
        ar.page.list.count = 4;
        ar.page.list.sel = 1;
        ar.page.list.item = item;
        ar.name = "The Paula Channels";
        ar.line = "Group";
        ar.bio = artist_bio;
        ar.art = "cover-artist";
        ar.facts.n = 2;
        ar.facts.facts = artist_facts;
        return &ar;
    }
    if (screen == LS_HOME) {
        memset(&home, 0, sizeof(home));
        home.n = 9;
        home.sel = 5;
        home.sections = secs;
        home.names = mods;
        home.nrows = 1;
        home.rows = &row;
        home.tile_sel = home_tile;
        home.tile_shown = 1;
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
        now.volume = 44;
        now.btn = LNB_PLAY;
        now.off = LNB_BIT(LNB_BACK) | LNB_BIT(LNB_FWD);
        now.on = LNB_BIT(LNB_LOOP);
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
        page.letter = 'P';
    } else if (scene == 3) {
        page.section = LSEC_SETTINGS;   /* Plex's servers being looked for */
        page.ncrumb = 3;
        page.crumb[0] = "Settings";
        page.crumb[1] = "Plex";
        page.crumb[2] = "Plex server";
        page.message = "Looking for Plex servers...  Esc to give up";
        page.waiting = "Searching...";
        page.wait_phase = 3;
    } else {
        page.section = LSEC_OTHER;
        page.ncrumb = 2;
        page.crumb[0] = "About";
        page.crumb[1] = "Ambiance";
        page.text = 1;
        page.text_frac = text_frac;
        page.ndetails = sizeof(text_lines) / sizeof(text_lines[0]);
        page.details = text_lines;
        page.message = "Press Esc again to quit Ambiance";
        page.picture = "shot-pinball";
        page.note = "This front-end would not be possible without the immense efforts of the AGS team.";
        page.plexmark = 1;
    }
    return &page;
}

/* what the layout asked for, as it asked */
static int acts[64][2], nacts;
/* a stand-in big letter: a block 40 x 80 */
static WORD h_letter(WORD xr, WORD ym, ULONG fg)
{
    h_fill((WORD)(xr - 40), (WORD)(ym - 40), 40, 80, fg);
    return 40;
}

/* a stand-in logo: 120 x 30, a blue box and white */
static const struct LImage *h_logo(ULONG bg)
{
    static ULONG lp[120 * 30];
    static struct LImage im = { 120, 30, 120, lp };
    int i;
    for (i = 0; i < 120 * 30; i++)
        lp[i] = (i % 120) < 30 ? 0x2266CC : (i / 120 > 8 && i / 120 < 22) ? 0xFFFFFF : bg;
    return &im;
}

/* a stand-in Plex mark: 86 x 40, yellow */
static const struct LImage *h_plexmark(ULONG bg)
{
    static ULONG pp[86 * 40];
    static struct LImage im = { 86, 40, 86, pp };
    int i;
    for (i = 0; i < 86 * 40; i++)
        pp[i] = (i % 86) > 60 ? 0xEBAF00 : (i / 86 > 8 && i / 86 < 32) ? 0xFFFFFF : bg;
    return &im;
}

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
    sprintf(path, "build/" LNAME "-%s-%dx%d.ppm", name, SW, SH);
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
    static const char *names[] = { "settings", "games", "text", "waiting" };
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
    out.copy = s_copy;
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
    host.copy = h_copy;
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
    host.logo = h_logo;
    host.plexmark = h_plexmark;
    host.debug = "CPU \a12%  app  3%   RAM 41023K  (chip 812K)";
    host.letter = h_letter;
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
    home_tile = 4;                      /* the section's list in use: its tiles */
    drawn_px = 0;
    layout_engine.draw(LS_HOME);
    save("home-tiles");
    nacts = 0;
    CHECK(layout_engine.input(LS_HOME, LA_RIGHT, 0) == LR_DONE && nacts == 1 && acts[0][1] == 5);
    nacts = 0;
    CHECK(layout_engine.input(LS_HOME, LA_LEFT, 0) == LR_DONE && acts[0][1] == 3);
    home_tile = -1;
    for (scene = 0; scene < 4; scene++) {
        long full;
        drawn_px = 0;
        layout_engine.draw(LS_PAGE);
        full = drawn_px;
        CHECK(drawn_px >= (long)w * h);
        save(names[scene]);
        if (scene == 2) {               /* the text at its end: its last line shown */
            text_frac = 65536;
            layout_engine.draw(LS_PAGE);
            save("text-end");
            text_frac = 0;
            layout_engine.draw(LS_PAGE);
        }
        if (scene == 0) {               /* the list's bar: pressed, dragged, let go */
            int px, py = h / 2, found = 0, paged = 0, r;
            /* the bar: off its thumb a page that way, on it the thumb taken */
            for (px = w - 1; px > w / 2 && !found; px--) {
                nacts = 0;
                r = layout_engine.pointer(LS_PAGE, (WORD)px, (WORD)py, LP_PRESS);
                if (r == LR_DONE && nacts && acts[0][0] == LAC_SCROLLTO)
                    found = paged = 1;
                else if (r == LR_DONE && !nacts)
                    found = 1;
                else if (nacts)
                    layout_engine.draw(LS_PAGE);    /* (something else pressed: as it was) */
            }
            CHECK(found);
            px++;
            if (paged) {                /* the thumb, further up the bar */
                CHECK(acts[0][1] > 0);
                for (found = 0, py = h / 2; py > 0 && !found; py--) {
                    nacts = 0;
                    if (layout_engine.pointer(LS_PAGE, (WORD)px, (WORD)py, LP_PRESS) == LR_DONE && !nacts)
                        found = 1;
                    else if (nacts)
                        CHECK(acts[0][0] == LAC_SCROLLTO);      /* (above it: a page up) */
                }
                CHECK(found);
            }
            nacts = 0;
            CHECK(layout_engine.pointer(LS_PAGE, (WORD)px, (WORD)(h - 1), LP_MOVE) == LR_DONE && nacts == 1 &&
                  acts[0][0] == LAC_SCROLLTO && acts[0][1] == 65536);
            CHECK(layout_engine.pointer(LS_PAGE, (WORD)px, (WORD)(h - 1), LP_RELEASE) == LR_DONE);
            nacts = 0;
            layout_engine.pointer(LS_PAGE, (WORD)px, 2, LP_MOVE);
            CHECK(nacts == 0);          /* (let go: a move drags no more) */
            layout_engine.draw(LS_PAGE);
        }
        if (scene == 0) {               /* the pointer over a row: its glow */
            drawn_px = 0;
            CHECK(layout_engine.pointer(LS_PAGE, (WORD)(w * 6 / 10), (WORD)(h * 37 / 100), FALSE) == LR_DONE);
            CHECK(drawn_px > 0);
            save("hover");
            layout_engine.pointer(LS_PAGE, 2, 2, FALSE);
        }
        if (scene == 3) {               /* the spinner's step alone */
            drawn_px = 0;
            layout_engine.changed(LS_PAGE, LC_WAIT);
            CHECK(drawn_px > 0 && drawn_px < full / 20);
        }
        if (scene == 1) {               /* a move alone: the list and details drawn again */
            drawn_px = 0;
            layout_engine.changed(LS_PAGE, LC_SELECT);
            CHECK(drawn_px > 0 && drawn_px < full * 3 / 4);
        }
    }
    {
        static const int ms[3] = { LS_ALBUMS, LS_ALBUM, LS_ARTIST };
        static const char *const mn[3] = { "wall", "album", "artist" };
        int k;
        for (k = 0; k < 3; k++) {
            drawn_px = 0;
            layout_engine.draw(ms[k]);
            CHECK(drawn_px >= (long)w * h);
            save(mn[k]);
            drawn_px = 0;
            layout_engine.changed(ms[k], LC_SELECT);
            CHECK(drawn_px > 0);
        }
        frames_wall = 1;                /* Videos' wall: 4:3 frames */
        drawn_px = 0;
        layout_engine.draw(LS_ALBUMS);
        CHECK(drawn_px >= (long)w * h);
        save("frames");
        frames_wall = 2;
        layout_engine.draw(LS_ALBUMS);
        save("games-wall");
        frames_wall = 0;
        /* the wall's keys: Right a cover, Down a row, Left at the edge back */
        layout_engine.draw(LS_ALBUMS);
        nacts = 0;
        CHECK(layout_engine.input(LS_ALBUMS, LA_RIGHT, 0) == LR_DONE && nacts == 1 &&
              acts[0][0] == LAC_SELECT && WALL_RIGHT(acts[0][1]));
        nacts = 0;
        CHECK(layout_engine.input(LS_ALBUMS, LA_DOWN, 0) == LR_DONE && nacts == 1 &&
              acts[0][0] == LAC_SELECT && acts[0][1] > 6);
        CHECK(layout_engine.input(LS_ALBUMS, LA_FIRE, 0) == LR_UNUSED);
        {
            /* a move to the far end: the wall glides there (Horizon: what
             * is drawn moved along, the rest drawn in), and comes out as
             * drawn whole there */
            unsigned long *was;
            long n = (long)w * h, k, diff = 0;
            int t;
            layout_engine.draw(LS_ALBUMS);
            wall_sel = (LONG)(sizeof(album_rows) / sizeof(album_rows[0])) - 1;
            copied_px = 0;
            layout_engine.changed(LS_ALBUMS, LC_SELECT);
            for (t = 0; t < 30; t++) {
                clock_ms += 40;
                layout_engine.tick(clock_ms);
            }
            save("wall-glided");
            was = malloc((size_t)n * sizeof(*was));
            memcpy(was, px, (size_t)n * sizeof(*was));
            layout_engine.draw(LS_ALBUMS);
            save("wall-whole");
            for (k = 0; k < n; k++)
                if (was[k] != px[k])
                    diff++;
            if (diff)
                printf("  (the glided wall: %ld pixels unlike the wall drawn whole)\n", diff);
            CHECK(diff == 0);
#ifdef HORIZON
            CHECK(copied_px > 0);
#endif
            free(was);
            wall_sel = 6;
        }
#ifdef HORIZON
        {
            /* Home's row: a tile far along chosen, the row glides there
             * (moved along by copies), and comes out as drawn whole */
            unsigned long *was;
            long n = (long)w * h, k, diff = 0;
            int t;
            home_tile = 0;
            layout_engine.draw(LS_HOME);
            home_tile = 5;
            copied_px = 0;
            layout_engine.changed(LS_HOME, LC_SELECT);
            for (t = 0; t < 30; t++) {
                clock_ms += 40;
                layout_engine.tick(clock_ms);
            }
            save("home-glided");
            was = malloc((size_t)n * sizeof(*was));
            memcpy(was, px, (size_t)n * sizeof(*was));
            layout_engine.draw(LS_HOME);
            for (k = 0; k < n; k++)
                if (was[k] != px[k])
                    diff++;
            if (diff)
                printf("  (the glided row: %ld pixels unlike Home drawn whole)\n", diff);
            CHECK(diff == 0 && copied_px > 0);
            free(was);
            home_tile = -1;             /* (back to the menu: the row glides home) */
            layout_engine.draw(LS_HOME);
            for (t = 0; t < 30; t++) {
                clock_ms += 40;
                layout_engine.tick(clock_ms);
            }
        }
#endif
    }
    drawn_px = 0;
    layout_engine.draw(LS_NOW);
    CHECK(drawn_px >= (long)w * h);
    {
        /* the time going on: it alone, not the screen, and as the screen
         * drawn whole has it (Now playing; a page: the top right or inset) */
        static const int scr[2] = { LS_NOW, LS_PAGE };
        long n = (long)w * h, k, diff;
        unsigned long *was = malloc((size_t)n * sizeof(*was));
        int q;
        for (q = 0; q < 2; q++) {
            layout_engine.draw(scr[q]);
            memcpy(was, px, (size_t)n * sizeof(*was));
            drawn_px = 0;
            layout_engine.changed(scr[q], LC_PLAYING);
            CHECK(drawn_px > 0 && drawn_px < (long)w * h / 8);
            for (diff = 0, k = 0; k < n; k++)
                if (was[k] != px[k])
                    diff++;
            if (diff) {
                long x0 = w, y0 = h, x1 = -1, y1 = -1;
                for (k = 0; k < n; k++)
                    if (was[k] != px[k]) {
                        if (k % w < x0) x0 = k % w;
                        if (k % w > x1) x1 = k % w;
                        if (k / w < y0) y0 = k / w;
                        if (k / w > y1) y1 = k / w;
                    }
                printf("  (the time drawn alone, screen %d: %ld pixels unlike the screen drawn whole, %ld,%ld-%ld,%ld)\n",
                       scr[q], diff, x0, y0, x1, y1);
            }
            CHECK(diff == 0);
        }
        free(was);
    }
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
            CHECK((a == last + 1 || (HOME_SOME && last < 0)) && nacts == 2 && acts[1][0] == LAC_ENTER);
            last = a;
            seen++;
        }
    CHECK(HOME_SOME ? seen >= 5 : seen == 9);
#ifdef HORIZON
    {
        /* Home's row: dragged along by the mouse (nothing chosen), and a
         * tile pressed and let go where it was: chosen */
        int tx = 420 * w / 800, ty = 232 * h / 600;
        layout_engine.draw(LS_HOME);
        nacts = 0;
        CHECK(layout_engine.pointer(LS_HOME, (WORD)tx, (WORD)ty, LP_PRESS) == LR_DONE && !nacts);
        drawn_px = 0;
        CHECK(layout_engine.pointer(LS_HOME, (WORD)(tx - 150 * w / 800), (WORD)ty, LP_MOVE) == LR_DONE &&
              drawn_px > 0);
        save("home-dragged");
        CHECK(layout_engine.pointer(LS_HOME, (WORD)(tx - 150 * w / 800), (WORD)ty, LP_RELEASE) == LR_DONE &&
              !nacts);
        layout_engine.pointer(LS_HOME, (WORD)tx, (WORD)ty, LP_PRESS);
        CHECK(layout_engine.pointer(LS_HOME, (WORD)tx, (WORD)ty, LP_RELEASE) == LR_DONE && nacts == 2 &&
              acts[0][0] == LAC_ENTER);
    }
#endif
#ifdef HORIZON
    /* (Home's Music row has what plays as a tile: the inset is on the pages) */
    layout_engine.draw(LS_PAGE);
    for (ok = 0, x = w / 2; x < w && !ok; x++)
        ok = click_at(LS_PAGE, x, NOW_Y, NULL) == LAC_NOWPLAYING;
#else
    for (ok = 0, x = w / 2; x < w && !ok; x++)
        ok = click_at(LS_HOME, x, NOW_Y, NULL) == LAC_NOWPLAYING;
#endif
    CHECK(ok);
    scene = 0;                              /* Settings > Display: a row opens */
    layout_engine.draw(LS_PAGE);
    for (ok = 0, y = 0; y < h && !ok; y++)
        ok = click_at(LS_PAGE, 500 * w / 800, y, &a) == LAC_OPEN;
    CHECK(ok);
    scene = 1;                              /* a game: selected, then played */
    layout_engine.draw(LS_PAGE);
    for (ok = 0, y = 0; y < h && !ok; y++)  /* (the list at the left: its details beside it) */
        if (click_at(LS_PAGE, 200 * w / 800, y, &a) == LAC_SELECT && a == 8) {
            clock_ms += 200;                /* a double-click */
            nacts = 0;
            layout_engine.pointer(LS_PAGE, (WORD)(200 * w / 800), (WORD)y, TRUE);
            ok = nacts == 1 && acts[0][0] == LAC_OPEN && acts[0][1] == 8;
        }
    CHECK(ok);
    for (ok = 0, y = 0; y < h / 8 && !ok; y += 2)  /* the crumb "All games", at the top: back once */
        for (x = w / 5; x < w / 2 && !ok; x += 3)
            if (click_at(LS_PAGE, x, y, NULL) == LAC_BACK)
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
    printf(LTITLE " drawn: 5 screens at 800x600/24, 640x480/16, 1024x768/32; clicks\n");
    return 0;
}
