/*
 * Ambiance - the ZZ9000's zz9k.library: its media sessions (MPEG video
 * decoded on the card, shown on its overlay, its sound given back)
 * Copyright (c) 2026 Brian King, MIT licence
 *
 * The only file built with the ZZ9000 SDK's headers (MIT, fetched by the
 * Makefile: third_party/zz9k-sdk.txt; zz9k.library itself is GPL-3, with a
 * section 7 permission for any program that opens it and calls it through
 * these headers). Its request builders and reply decoders
 * (zz9k/request.h, reply.h) make each call; zz9k.library carries it
 * (ZZ9KCall; before 2.34, zz_call's own wait), shared safely with the
 * card's picture datatype, mpega and AmiSSL. The UI and the playback process both call it: the library
 * serialises the mailbox.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "proto/zz9k.h"
#include "zz9k/request.h"
#include "zz9k/reply.h"
#include "amce.h"
#include "zz9k.h"

struct Library *ZZ9KBase;
static int zz_opens;

BOOL zz_open(void)
{
    if (zz_opens++ > 0)
        return ZZ9KBase != NULL;
    ZZ9KBase = OpenLibrary((CONST_STRPTR)ZZ9K_LIBRARY_NAME, ZZ9K_LIBRARY_VERSION);
    if (!ZZ9KBase)
        amce_log("zz9k", "zz9k.library 2 could not be opened");
    return ZZ9KBase != NULL;
}

void zz_close(void)
{
    if (zz_opens <= 0 || --zz_opens > 0)
        return;
    if (ZZ9KBase)
        CloseLibrary(ZZ9KBase);
    ZZ9KBase = NULL;
}

const char *zz_name(int s)
{
    static const char *const names[] = { "OK", "queued", "busy", "unsupported", "bad request",
                                         "bad handle", "no memory", "timeout", "cancelled",
                                         "I/O error", "not found", "module missing",
                                         "module mismatch" };
    if (s >= 0 && s < (int)(sizeof(names) / sizeof(names[0])))
        return names[s];
    return s == ZZ9K_STATUS_INTERNAL_ERROR ? "internal error" : "?";
}

const char *zz_video_check(BOOL mpeg2, BOOL mp2)
{
    static char said;
    ZZ9KCaps caps;
    ZZ9KServiceInfo svc;
    ULONG need = ZZ9K_SERVICE_FLAG_VIDEO_DIRECT_OVERLAY | ZZ9K_SERVICE_FLAG_VIDEO_STREAMING_INPUT |
                 ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_SESSION | ZZ9K_SERVICE_FLAG_VIDEO_MPEG_PS |
                 (mpeg2 ? ZZ9K_SERVICE_FLAG_VIDEO_MPEG2 : ZZ9K_SERVICE_FLAG_VIDEO_MPEG1) |
                 (mp2 ? ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_MP2 : 0);
    char m[200];
    int s;
    if (!ZZ9KBase)
        return "zz9k.library is not there";
    memset(&caps, 0, sizeof(caps));
    memset(&svc, 0, sizeof(svc));
    amce_card_lock();
    s = ZZ9KQueryCaps(&caps);
    amce_card_unlock();
    if (s != ZZ9K_STATUS_OK) {
        sprintf(m, "the card's capabilities could not be read (%s)", zz_name(s));
        amce_log("zz9k", m);
        return "The ZZ9000 did not answer";
    }
    amce_card_lock();
    s = ZZ9KQueryService(ZZ9K_SERVICE_VIDEO, &svc);
    amce_card_unlock();
    if (!said) {
        said = 1;
        sprintf(m, "zz9k.library %ld.%ld, ABI %ld.%ld, firmware %08lx, caps %08lx; video service "
                   "%s, flags %08lx%s",
                (long)ZZ9KBase->lib_Version, (long)ZZ9KBase->lib_Revision, (long)caps.abi_major,
                (long)caps.abi_minor, (unsigned long)caps.firmware_version,
                (unsigned long)caps.capability_bits, s == ZZ9K_STATUS_OK ? "there" : zz_name(s),
                (unsigned long)svc.flags,
                ZZ9KBase->lib_Version == 2 && ZZ9KBase->lib_Revision >= 2 && ZZ9KBase->lib_Revision < 34
                    ? "; its replies waited for by Ambiance (before 2.34 its timeout is 250 polls)" : "");
        amce_log("zz9k", m);
    }
    if ((caps.capability_bits & (ZZ9K_CAP_VIDEO_DECODE | ZZ9K_CAP_MEDIA_SESSION)) !=
            (ZZ9K_CAP_VIDEO_DECODE | ZZ9K_CAP_MEDIA_SESSION) ||
        s != ZZ9K_STATUS_OK)
        return "This ZZ9000's firmware plays no video";
    if ((svc.flags & need) != need) {
        if (!(svc.flags & (mpeg2 ? ZZ9K_SERVICE_FLAG_VIDEO_MPEG2 : ZZ9K_SERVICE_FLAG_VIDEO_MPEG1)))
            return mpeg2 ? "This ZZ9000's firmware plays no MPEG-2" : "This ZZ9000's firmware plays no MPEG-1";
        if (mp2 && !(svc.flags & ZZ9K_SERVICE_FLAG_VIDEO_MEDIA_MP2))
            return "This ZZ9000's firmware has no MPEG sound for video";
        return "This ZZ9000's firmware has no media sessions for video";
    }
    return NULL;
}

int zz_alloc(ULONG length, struct ZzBuf *b)
{
    ZZ9KSharedBuffer sb;
    int s;
    memset(&sb, 0, sizeof(sb));
    memset(b, 0, sizeof(*b));
    amce_card_lock();
    s = ZZ9KAllocShared(length, 64, ZZ9K_ALLOC_HOST_WINDOW, &sb);
    if (s == ZZ9K_STATUS_OK && !sb.data) {
        if (sb.handle)
            ZZ9KFreeShared(sb.handle);
        amce_card_unlock();
        return ZZ9K_STATUS_NO_MEMORY;
    }
    amce_card_unlock();
    b->handle = sb.handle;
    b->data = (volatile UBYTE *)sb.data;
    b->length = sb.length;
    return s;
}

void zz_free(struct ZzBuf *b)
{
    if (b->handle) {
        amce_card_lock();
        ZZ9KFreeShared(b->handle);
        amce_card_unlock();
    }
    memset(b, 0, sizeof(*b));
}

static void main_out(const ZZ9KMediaSessionMainResult *a, struct ZzMain *r)
{
    r->session = a->session;
    r->state = a->state;
    r->width = a->width;
    r->height = a->height;
    r->rate_num = a->frame_rate_num;
    r->rate_den = a->frame_rate_den;
    r->frame = a->frame_number;
    r->pts = a->video_pts;
    r->accepted = a->bytes_accepted;
    r->written = a->bytes_written;
    r->flags = a->flags;
}

/* A request sent and its reply waited for, under the card lock.
 * zz9k.library before 2.34 counted ZZ9KCall's timeout in tight polls of
 * the mailbox (250 of them: a few milliseconds), not in DOS ticks as it
 * says: a reply the card was a little slower over (a PRESENT while it
 * composes the last picture) came back as a timeout, and the video
 * stopped. There it is sent asynchronously and polled here: tightly at
 * first, then a tick at a time, ZZ_WAIT_TICKS at most (and taken off the
 * library's list if it never comes, as it lives on this stack). */
#define ZZ_SPIN_POLLS 400
#define ZZ_WAIT_TICKS 150                   /* (three seconds) */
static int zz_call(ZZ9KRequest *rq, ZZ9KMailboxEntry *reply)
{
    ZZ9KAsyncRequest a;
    ULONG n, done;
    int s;
    if (ZZ9KBase->lib_Version > 2 || ZZ9KBase->lib_Revision >= 34 || ZZ9KBase->lib_Revision < 2) {
        amce_card_lock();                   /* (2.34 on: its timeout is ticks; before 2.2 no cancel) */
        s = ZZ9KCall(rq, reply, ZZ9K_DEFAULT_TIMEOUT_TICKS);
        amce_card_unlock();
        return s;
    }
    memset(&a, 0, sizeof(a));
    amce_card_lock();
    s = ZZ9KCallAsync(&a, rq, NULL, NULL);
    if (s != ZZ9K_STATUS_QUEUED) {
        amce_card_unlock();
        return s;
    }
    for (n = 0;; n++) {
        done = 0;
        s = ZZ9KPoll(8, &done);
        if (a.completed) {
            *reply = a.reply;
            s = a.status;
            break;
        }
        if (s != ZZ9K_STATUS_OK)
            break;
        if (n >= ZZ_SPIN_POLLS + ZZ_WAIT_TICKS) {
            s = ZZ9K_STATUS_TIMEOUT;
            break;
        }
        if (n >= ZZ_SPIN_POLLS)
            Delay(1);
    }
    if (a.queued && !a.completed)
        ZZ9KCancelAsync(&a);
    amce_card_unlock();
    return s;
}

/* A request sent, its main result read back. */
static int main_call(ZZ9KRequest *rq, UWORD op, struct ZzMain *r)
{
    ZZ9KMailboxEntry reply;
    ZZ9KMediaSessionMainResult res;
    int s;
    memset(&reply, 0, sizeof(reply));
    memset(r, 0, sizeof(*r));
    s = zz_call(rq, &reply);
    if (s != ZZ9K_STATUS_OK)
        return s;
    if ((s = zz9k_reply_media_session_main(&reply, op, &res)) != ZZ9K_STATUS_OK)
        return s;
    main_out(&res, r);
    return ZZ9K_STATUS_OK;
}

int zz_begin(ULONG w, ULONG h, BOOL mpeg2, int audio, const struct ZzBuf *pcm, struct ZzMain *r)
{
    ZZ9KMediaSessionBeginDesc d;
    ZZ9KRequest rq;
    int s;
    memset(&d, 0, sizeof(d));
    d.video_codec = mpeg2 ? ZZ9K_VIDEO_CODEC_MPEG2 : ZZ9K_VIDEO_CODEC_MPEG1;
    d.container = ZZ9K_VIDEO_CONTAINER_MPEG_PS;
    d.width = w;
    d.height = h;
    d.output_format = ZZ9K_VIDEO_OUTPUT_DIRECT_OVERLAY;
    d.audio_codec = audio == ZZ_AUDIO_AC3 ? ZZ9K_MEDIA_AUDIO_AC3
                    : audio == ZZ_AUDIO_MP2 ? ZZ9K_MEDIA_AUDIO_MP2 : ZZ9K_MEDIA_AUDIO_NONE;
    if (audio && pcm) {
        ULONG high = pcm->length - pcm->length / 4, low = pcm->length / 4;
        d.pcm_ring_handle = pcm->handle;
        d.pcm_ring_capacity = pcm->length;
        d.pcm_low_water_bytes = low;
        d.pcm_high_water_bytes = high;
    }
    if ((s = zz9k_request_media_session_begin(&rq, &d)) != ZZ9K_STATUS_OK)
        return s;
    return main_call(&rq, ZZ9K_OP_MEDIA_SESSION_BEGIN, r);
}

int zz_write(ULONG session, const struct ZzBuf *in, ULONG off, ULONG len, BOOL eof,
             struct ZzMain *r)
{
    ZZ9KMediaSessionWriteDesc d;
    ZZ9KRequest rq;
    int s;
    memset(&d, 0, sizeof(d));
    d.session = session;
    d.src_handle = in->handle;
    d.src_offset = off;
    d.src_length = len;
    d.flags = eof ? ZZ9K_MEDIA_SESSION_WRITE_EOF : 0;
    if ((s = zz9k_request_media_session_write(&rq, &d)) != ZZ9K_STATUS_OK)
        return s;
    return main_call(&rq, ZZ9K_OP_MEDIA_SESSION_WRITE, r);
}

int zz_cmd(int op, ULONG session, ULONG flags, struct ZzMain *r)
{
    static const UWORD ops[] = { ZZ9K_OP_MEDIA_SESSION_DECODE, ZZ9K_OP_MEDIA_SESSION_PRESENT,
                                 ZZ9K_OP_MEDIA_SESSION_DISCARD, ZZ9K_OP_MEDIA_SESSION_CLOSE };
    ZZ9KRequest rq;
    int s;
    if ((s = zz9k_request_media_session_command(&rq, ops[op], session, flags)) != ZZ9K_STATUS_OK)
        return s;
    return main_call(&rq, ops[op], r);
}

int zz_audio_read(ULONG session, ZzU64 acknowledged, struct ZzAudio *r)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry reply;
    ZZ9KMediaSessionAudioResult a;
    int s;
    memset(r, 0, sizeof(*r));
    if ((s = zz9k_request_media_session_command_value(&rq, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ,
                                                      session, acknowledged, 0)) != ZZ9K_STATUS_OK)
        return s;
    memset(&reply, 0, sizeof(reply));
    s = zz_call(&rq, &reply);
    if (s != ZZ9K_STATUS_OK)
        return s;
    if ((s = zz9k_reply_media_session_audio(&reply, ZZ9K_OP_MEDIA_SESSION_AUDIO_READ, &a)) !=
        ZZ9K_STATUS_OK)
        return s;
    r->session = a.session;
    r->state = a.state;
    r->rate = a.sample_rate;
    r->channels = a.channels;
    r->format = a.sample_format;
    r->produced = a.pcm_produced;
    r->acknowledged = a.pcm_acknowledged;
    r->pts = a.audio_pts;
    r->flags = a.flags;
    return ZZ9K_STATUS_OK;
}

int zz_audio_origin(ULONG session, ZzU64 *pts)
{
    ZZ9KRequest rq;
    ZZ9KMailboxEntry reply;
    ZZ9KMediaSessionStatusResult res;
    int s;
    *pts = ZZ_NO_PTS;
    if ((s = zz9k_request_media_session_status(&rq, session, ZZ9K_MEDIA_STATUS_AUDIO, 0)) !=
        ZZ9K_STATUS_OK)
        return s;
    memset(&reply, 0, sizeof(reply));
    s = zz_call(&rq, &reply);
    if (s != ZZ9K_STATUS_OK)
        return s;
    if ((s = zz9k_reply_media_session_status(&reply, &res)) != ZZ9K_STATUS_OK)
        return s;
    *pts = res.value[3];
    return ZZ9K_STATUS_OK;
}
