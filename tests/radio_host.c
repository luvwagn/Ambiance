/* Host test for radiocsv.c: CSV rows, UTF-8 to Latin-1, URL encoding. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/radiocsv.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct ctx {
    int rows;
    int c_name, c_url, c_bitrate;
    char names[8][64], urls[8][128];
    int bitrates[8];
    int nfields[8];
};

static int row(void *v, char **f, int n)
{
    struct ctx *c = v;
    if (c->rows == 0) {
        c->c_name = csv_col(f, n, "name");
        c->c_url = csv_col(f, n, "url_resolved");
        c->c_bitrate = csv_col(f, n, "bitrate");
    } else {
        int r = c->rows - 1;
        if (r < 8) {
            strcpy(c->names[r], f[c->c_name]);
            strcpy(c->urls[r], f[c->c_url]);
            c->bitrates[r] = atoi(f[c->c_bitrate]);
            c->nfields[r] = n;
        }
    }
    c->rows++;
    return 1;
}

int main(void)
{
    static const char csv[] =
        "changeuuid,stationuuid,name,url,url_resolved,tags,bitrate\r\n"
        "a,b,Plain Radio,http://x/s,http://x/s.mp3,\"rock,pop\",128\r\n"
        "c,d,\"Quoted, \"\"Name\"\"\",http://y,http://y/live,,64\r\n"
        "e,f,\"Multi\nLine\",https://z,https://z/a,chiptune,0\n"
        "g,h,Caf\xC3\xA9 \xE2\x80\x9CRadio\xE2\x80\x9D \xF0\x9F\x8E\xB5,http://q,http://q,,192";
    char *buf = malloc(sizeof(csv) + 1);
    struct ctx c;
    char enc[64];
    long rows;
    memcpy(buf, csv, sizeof(csv));
    memset(&c, 0, sizeof(c));
    rows = csv_parse(buf, sizeof(csv) - 1, row, &c);
    CHECK(rows == 5);
    CHECK(c.c_name == 2 && c.c_url == 4 && c.c_bitrate == 6);
    CHECK(!strcmp(c.names[0], "Plain Radio"));
    CHECK(c.bitrates[0] == 128);
    CHECK(!strcmp(c.names[1], "Quoted, \"Name\""));
    CHECK(!strcmp(c.urls[1], "http://y/live"));
    CHECK(c.bitrates[1] == 64);
    CHECK(!strcmp(c.names[2], "Multi\nLine"));
    CHECK(!strcmp(c.urls[2], "https://z/a"));
    CHECK(c.nfields[3] == 7);
    CHECK(c.bitrates[3] == 192);
    utf8_to_latin1(c.names[3]);
    CHECK(!strcmp(c.names[3], "Caf\xE9 \"Radio\" ?"));
    squeeze_spaces(c.names[2]);
    CHECK(!strcmp(c.names[2], "Multi Line"));
    {
        /* Greek kept as cjk.c's two bytes, Japanese as its three */
        char gr[] = "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1 FM \xCF\x8E";
        char jp[] = "\xE3\x83\xA9\xCE\xB1";
        utf8_to_latin1(gr);
        CHECK(!strcmp(gr, "\x80\x95\x80\xBB\x80\xBB\x80\xAC\x80\xB4\x80\xB1 FM \x81\x8E"));
        utf8_to_latin1(jp);
        CHECK(!strcmp(jp, "\x86\xA3\xA9\x80\xB1"));
    }
    url_encode("drum and bass/ü", enc, sizeof(enc));
    CHECK(!strcmp(enc, "drum%20and%20bass%2F%C3%BC"));
    free(buf);
    if (!fails)
        printf("radio host tests passed\n");
    return fails != 0;
}
