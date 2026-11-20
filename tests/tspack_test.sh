#!/bin/sh
# tspack.c on the host against ffmpeg: transport streams (188-byte, and
# 192-byte M2TS) made by ffmpeg, packed as Program Streams by tspack.c;
# the video read from each by ffmpeg must be the same bytes, the sound
# the TS's from where the PS's begins; and two packed at once, each by an
# instance of its own, the same as each alone.
# With mpegv's host test (MPV), its pictures decoded by mpegv too: all of
# them, and from the middle (a seek: from the next sequence header on).
#   tests/tspack_test.sh BUILD_DIR HOST_PROGRAM [MPV]
set -e
D="$1"
P="$2"
MPV="$3"
mkdir -p "$D"
cd "$D"
q="-hide_banner -loglevel error -y"
ffmpeg $q -f lavfi -i testsrc=size=352x240:rate=25 -f lavfi -i sine=frequency=440 -t 4 \
    -c:v mpeg1video -b:v 1500k -g 12 -bf 2 -c:a mp2 -b:a 192k -f mpegts a.ts
ffmpeg $q -f lavfi -i smptebars=size=320x240:rate=25 -f lavfi -i sine=frequency=660 -t 3 \
    -c:v mpeg1video -b:v 1000k -g 15 -c:a mp2 -f mpegts b.ts
ffmpeg $q -f lavfi -i testsrc2=size=720x576:rate=25 -f lavfi -i sine=frequency=330 -t 3 \
    -c:v mpeg2video -b:v 4M -g 12 -bf 2 -c:a mp2 -f mpegts -mpegts_m2ts_mode 1 c.m2ts
fail=0
check() {   # IN OUT KIND
    ffmpeg $q -i "$1" -map 0:v -c copy -f "$3" v_ts.es
    ffmpeg $q -i "$2" -map 0:v -c copy -f "$3" v_ps.es
    if cmp -s v_ts.es v_ps.es; then echo "  $1: video the same ($(wc -c < v_ps.es) bytes)";
    else echo "  $1: VIDEO DIFFERS ($(wc -c < v_ts.es) / $(wc -c < v_ps.es))"; fail=1; fi
    ffmpeg $q -i "$1" -map 0:a -c copy -f mp2 a_ts.es
    ffmpeg $q -i "$2" -map 0:a -c copy -f mp2 a_ps.es
    n=$(wc -c < a_ps.es); t=$(wc -c < a_ts.es)
    if [ "$n" -gt 0 ] && tail -c "$n" a_ts.es | cmp -s - a_ps.es; then
        echo "  $1: sound the same from the PS's start ($n of $t bytes)"
    else echo "  $1: SOUND DIFFERS ($t / $n)"; fail=1; fi
    frames=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$2")
    echo "  $1: ${frames%,} pictures decoded by ffmpeg from the PS"
}
"$P" a.ts a.mpg 188 256 257 0 > /dev/null
check a.ts a.mpg mpeg1video
"$P" c.m2ts c.mpg 192 4113 4352 1 > /dev/null
check c.m2ts c.mpg mpeg2video
"$P" a.ts a2.mpg 188 256 257 0 b.ts b2.mpg > /dev/null
"$P" b.ts b.mpg 188 256 257 0 > /dev/null
if cmp -s a.mpg a2.mpg && cmp -s b.mpg b2.mpg; then echo "  two at once, each its own: the same as each alone";
else echo "  TWO AT ONCE DIFFER"; fail=1; fi
if [ -n "$MPV" ] && [ -x "$MPV" ]; then
    for f in a c; do
        want=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 $f.mpg)
        got=$("$MPV" $f.mpg | tail -1 | cut -d' ' -f1)
        if [ "$got" = "${want%,}" ]; then echo "  $f: mpegv decodes all $got pictures from the PS";
        else echo "  $f: MPEGV DECODED $got OF ${want%,}"; fail=1; fi
    done
    # its start (the sequence header: ffmpeg's MPEG-1 has it there alone),
    # then a seek: the pictures after it decoded too
    TSP_HEAD=20000 TSP_SKIP=99999999 "$P" a.ts a_head.mpg 188 256 257 0 > /dev/null
    TSP_HEAD=20000 TSP_SKIP=150000 "$P" a.ts a_mid.mpg 188 256 257 0 > /dev/null
    TSP_HEAD=0 TSP_SKIP=150000 "$P" a.ts a_cold.mpg 188 256 257 0 > /dev/null
    TSP_HEAD=60000 TSP_SKIP=99999999 "$P" c.m2ts c_head.mpg 192 4113 4352 1 > /dev/null
    TSP_HEAD=60000 TSP_SKIP=900000 "$P" c.m2ts c_mid.mpg 192 4113 4352 1 > /dev/null
    TSP_HEAD=0 TSP_SKIP=900000 "$P" c.m2ts c_cold.mpg 192 4113 4352 1 > /dev/null
    for f in a c; do
        h=$("$MPV" ${f}_head.mpg | tail -1 | cut -d' ' -f1)
        got=$("$MPV" ${f}_mid.mpg | tail -1 | cut -d' ' -f1)
        cold=$("$MPV" ${f}_cold.mpg | tail -1 | cut -d' ' -f1)
        if [ "$got" -gt $((h + 10)) ] 2>/dev/null; then
            echo "  $f: its start, then a seek (tsp_seek_in): mpegv decodes $got pictures ($h before the seek)"
        else echo "  $f: AFTER A SEEK, MPEGV DECODED $got ($h before it)"; fail=1; fi
        if [ "$cold" -gt 10 ] 2>/dev/null; then
            echo "  $f: from the middle cold (tsp_begin: its next sequence header): $cold pictures"
        else echo "  $f: FROM THE MIDDLE COLD, MPEGV DECODED $cold"; fail=1; fi
    done
fi
[ $fail = 0 ] && echo "tspack tests passed"
exit $fail
