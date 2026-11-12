/* Host test for plexxml.c: a Plex server's XML, as AmigaMCE reads it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/plexxml.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

struct ctx {
    int n;
    char names[16][24], titles[16][80], keys[16][120];
    int depth[16];
};

static int el(void *v, const char *name, char **a, int n, int depth)
{
    struct ctx *c = v;
    const char *t = xml_attr(a, n, "title"), *k = xml_attr(a, n, "key");
    if (c->n < 16) {
        strcpy(c->names[c->n], name);
        strcpy(c->titles[c->n], t ? t : "");
        strcpy(c->keys[c->n], k ? k : "");
        c->depth[c->n] = depth;
    }
    c->n++;
    return 1;
}

int main(void)
{
    static const char xml[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MediaContainer size=\"2\" allowSync=\"1\" title1=\"Music\">\n"
        "<!-- a comment with <tags> in it -->\n"
        "<Directory ratingKey=\"12\" key=\"/library/metadata/12/children\" type=\"album\" "
        "title=\"Rock &amp; Roll &#233;t&#xE9; &quot;live&quot;\" parentTitle=\"Art'ist\" year=\"1987\"/>\n"
        "<Track ratingKey=\"34\" key=\"/library/metadata/34\" title=\"Song &lt;1&gt;\" duration=\"215000\">\n"
        "  <Media id=\"1\" duration=\"215000\" bitrate=\"320\" audioCodec=\"mp3\" container=\"mp3\">\n"
        "    <Part id=\"9\" key=\"/library/parts/9/1690000000/file.mp3\" size=\"8600000\" container=\"mp3\" />\n"
        "  </Media>\n"
        "  <Genre tag=\"Synth\"/>\n"
        "</Track>\n"
        "<Directory key='3' type='artist' title='Single quotes'></Directory>\n"
        "</MediaContainer>\n";
    char *buf = malloc(sizeof(xml) + 1);
    struct ctx c;
    long seen;
    memcpy(buf, xml, sizeof(xml));
    memset(&c, 0, sizeof(c));
    seen = xml_scan(buf, sizeof(xml) - 1, el, &c);
    CHECK(seen == 7);
    CHECK(!strcmp(c.names[0], "MediaContainer") && c.depth[0] == 0);
    CHECK(!strcmp(c.names[1], "Directory") && c.depth[1] == 1);
    CHECK(!strcmp(c.titles[1], "Rock & Roll \xC3\xA9t\xC3\xA9 \"live\""));
    CHECK(!strcmp(c.keys[1], "/library/metadata/12/children"));
    CHECK(!strcmp(c.names[2], "Track") && c.depth[2] == 1 && !strcmp(c.titles[2], "Song <1>"));
    CHECK(!strcmp(c.names[3], "Media") && c.depth[3] == 2);
    CHECK(!strcmp(c.names[4], "Part") && c.depth[4] == 3 &&
          !strcmp(c.keys[4], "/library/parts/9/1690000000/file.mp3"));
    CHECK(!strcmp(c.names[5], "Genre") && c.depth[5] == 2);
    CHECK(!strcmp(c.names[6], "Directory") && c.depth[6] == 1 &&
          !strcmp(c.titles[6], "Single quotes") && !strcmp(c.keys[6], "3"));
    printf(fails ? "plex xml: %d failed\n" : "plex xml: all passed\n", fails);
    free(buf);
    {
        /* plex.tv: a code (XML and JSON), and an account's servers */
        static const char pin_xml[] =
            "<?xml version=\"1.0\"?>\n<pin id=\"123456789\" code=\"AB12\" product=\"AmigaMCE\" "
            "trusted=\"0\" clientIdentifier=\"amigamce-1\" expiresIn=\"900\" authToken=\"\"/>";
        static const char pin_json[] =
            "{\"id\":987654,\"code\":\"ZQ9X\",\"product\":\"AmigaMCE\",\"expiresIn\": 1800,"
            "\"authToken\":null,\"clientIdentifier\":\"amigamce-1\"}";
        static const char pin_done[] = "{\"id\":987654,\"code\":\"ZQ9X\",\"authToken\":\"tok-ABC_123\"}";
        static const char res[] =
            "<MediaContainer size=\"3\">\n"
            "<Device name=\"Synology &amp; co\" product=\"Plex Media Server\" provides=\"server\" "
            "owned=\"1\" accessToken=\"srv-token-1\" clientIdentifier=\"abc\">\n"
            "  <Connection protocol=\"https\" address=\"192.168.1.20\" port=\"32400\" "
            "uri=\"https://192-168-1-20.x.plex.direct:32400\" local=\"1\"/>\n"
            "  <Connection protocol=\"http\" address=\"192.168.1.20\" port=\"32400\" "
            "uri=\"http://192.168.1.20:32400\" local=\"1\"/>\n"
            "  <Connection protocol=\"https\" address=\"81.2.3.4\" port=\"40000\" local=\"0\"/>\n"
            "</Device>\n"
            "<Device name=\"Phone\" product=\"Plex for iOS\" provides=\"client,player\" "
            "accessToken=\"x\"/>\n"
            "<Device name=\"Friend's\" provides=\"server\" owned=\"0\" accessToken=\"srv-token-2\">\n"
            "  <Connection protocol=\"https\" address=\"fe80::1\" port=\"32400\" local=\"1\"/>\n"
            "  <Connection protocol=\"https\" address=\"10.0.0.5\" port=\"32401\" local=\"1\"/>\n"
            "</Device>\n"
            "<Device name=\"Away\" provides=\"server\" accessToken=\"srv-token-3\">\n"
            "  <Connection protocol=\"https\" address=\"5.6.7.8\" port=\"32400\" local=\"0\"/>\n"
            "</Device>\n"
            "</MediaContainer>\n";
        char v[64], out[512], *rb = malloc(sizeof(res));
        CHECK(plex_tv_value((const unsigned char *)pin_xml, "id", v, sizeof(v)) && !strcmp(v, "123456789"));
        CHECK(plex_tv_value((const unsigned char *)pin_xml, "code", v, sizeof(v)) && !strcmp(v, "AB12"));
        CHECK(plex_tv_value((const unsigned char *)pin_xml, "expiresIn", v, sizeof(v)) && !strcmp(v, "900"));
        CHECK(!plex_tv_value((const unsigned char *)pin_xml, "authToken", v, sizeof(v)));
        CHECK(plex_tv_value((const unsigned char *)pin_json, "id", v, sizeof(v)) && !strcmp(v, "987654"));
        CHECK(plex_tv_value((const unsigned char *)pin_json, "code", v, sizeof(v)) && !strcmp(v, "ZQ9X"));
        CHECK(plex_tv_value((const unsigned char *)pin_json, "expiresIn", v, sizeof(v)) && !strcmp(v, "1800"));
        CHECK(!plex_tv_value((const unsigned char *)pin_json, "authToken", v, sizeof(v)));
        CHECK(plex_tv_value((const unsigned char *)pin_done, "authToken", v, sizeof(v)) &&
              !strcmp(v, "tok-ABC_123"));
        CHECK(!plex_tv_value((const unsigned char *)pin_xml, "missing", v, sizeof(v)));
        memcpy(rb, res, sizeof(res));
        CHECK(plex_resource_lines((unsigned char *)rb, sizeof(res) - 1, out, sizeof(out)) == 2);
        CHECK(!strcmp(out, "192.168.1.20\t32400\tSynology & co\tsrv-token-1\n"
                           "10.0.0.5\t32401\tFriend's\tsrv-token-2\n"));
        free(rb);
    }
    return fails ? 1 : 0;
}
