"""Music files for the songs test (tests/test_songs.c): WAV, AIFF, 8SVX,
an MP3 with an ID3v2.3 tag and a Xing frame, a FLAC with its metadata,
and folders with and without music in them."""
import os, struct, sys

root = sys.argv[1]
def mk(p, data):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)

def chunk_le(t, d): return t + struct.pack('<I', len(d)) + d + (b'\0' if len(d) & 1 else b'')
def chunk_be(t, d): return t + struct.pack('>I', len(d)) + d + (b'\0' if len(d) & 1 else b'')

# WAV: 16-bit stereo 44100 Hz, 1 s
pcm = b'\x01\x00\xff\xff' * 44100
fmt = struct.pack('<HHIIHH', 1, 2, 44100, 44100 * 4, 4, 16)
body = b'WAVE' + chunk_le(b'fmt ', fmt) + chunk_le(b'LIST', b'INFOjunk') + chunk_le(b'data', pcm)
mk(f'{root}/Loose/tone.wav', b'RIFF' + struct.pack('<I', len(body)) + body)

# AIFF: 8-bit mono 22050 Hz, 0.5 s, with a NAME
def ext80(v):
    import math
    e = int(math.floor(math.log2(v)))
    m = int(v / 2 ** e * 2 ** 63)
    return struct.pack('>HQ', e + 16383, m)
comm = struct.pack('>hIh', 1, 11025, 8) + ext80(22050)
ssnd = struct.pack('>II', 0, 0) + b'\x10' * 11025
body = b'AIFF' + chunk_be(b'COMM', comm) + chunk_be(b'NAME', b'Aiff Song') + chunk_be(b'SSND', ssnd)
mk(f'{root}/Loose/beep.aiff', b'FORM' + struct.pack('>I', len(body)) + body)

# 8SVX: 8363 Hz, 1 s
vhdr = struct.pack('>IIIHBBI', 8363, 0, 32, 8363, 1, 0, 0x10000)
body = b'8SVX' + chunk_be(b'VHDR', vhdr) + chunk_be(b'NAME', b'Sample') + chunk_be(b'BODY', b'\x20' * 8363)
mk(f'{root}/Loose/sample.8svx', b'FORM' + struct.pack('>I', len(body)) + body)

# MP3: ID3v2.3 (title, artist, album, year, track, APIC), then a Xing frame
def frame(fid, data): return fid + struct.pack('>IH', len(data), 0) + data
png = b'\x89PNG\r\n\x1a\n' + b'P' * 200
frames = (frame(b'TIT2', b'\x00Song One') + frame(b'TPE1', b'\x01\xff\xfeA\x00r\x00t\x00') +
          frame(b'TALB', b'\x03Alb\xc3\xa9') + frame(b'TYER', b'\x001999') +
          frame(b'TRCK', b'\x003/9') + frame(b'APIC', b'\x00image/png\x00\x03desc\x00' + png))
size = len(frames) + 64
ss = bytes([(size >> 21) & 0x7f, (size >> 14) & 0x7f, (size >> 7) & 0x7f, size & 0x7f])
tag = b'ID3\x03\x00\x00' + ss + frames + b'\0' * 64
hdr = bytes([0xFF, 0xFB, 0x90, 0x00])      # MPEG 1 layer III, 128 kbit/s, 44100, stereo
xing = hdr + b'\0' * 32 + b'Xing' + struct.pack('>II', 1, 1000) + b'\0' * 300
mk(f'{root}/Artist/Album/01 song.mp3', tag + xing + b'\0' * 5000)
mk(f'{root}/Solo/solo.mp3', tag + xing + b'\0' * 5000)     # no cover file: its own

# FLAC: STREAMINFO (44100 Hz stereo 16-bit, 441000 samples), comment, picture
si = struct.pack('>HH', 4096, 4096) + b'\0' * 6
v = (44100 << 44) | (1 << 41) | (15 << 36) | 441000
si += v.to_bytes(8, 'big') + b'\0' * 16
vend = b'test'
comments = [b'TITLE=Flac Song', b'ARTIST=Flac Artist', b'ALBUM=Flac Album', b'DATE=2001']
vc = struct.pack('<I', len(vend)) + vend + struct.pack('<I', len(comments))
for c in comments:
    vc += struct.pack('<I', len(c)) + c
pic = struct.pack('>I', 3) + struct.pack('>I', 10) + b'image/jpeg' + struct.pack('>I', 0) + \
      struct.pack('>IIII', 1, 1, 24, 0) + struct.pack('>I', 300) + b'\xff\xd8' + b'J' * 298
def block(t, d, last=False): return bytes([t | (0x80 if last else 0)]) + len(d).to_bytes(3, 'big') + d
mk(f'{root}/Artist/Other/x.flac', b'fLaC' + block(0, si) + block(4, vc) + block(6, pic, True) + b'\0' * 100)

# folders without music: left out of the lists
mk(f'{root}/Empty/Deeper/readme.txt', b'nothing')
mk(f'{root}/Pics/a.jpg', b'\xff\xd8')
# a folder with a cover file
mk(f'{root}/Artist/Album/cover.png', b'\x89PNG')

# Kodi's .nfo files and pictures (nfo.c): an album's album.nfo, its
# artist's artist.nfo above it, and an album in disc folders (CD1, CD2)
mk(f'{root}/Artist/Album/album.nfo', '''<?xml version="1.0" encoding="UTF-8" standalone="yes" ?>
<!-- written by Kodi -->
<album>
    <title>Copper Lists</title>
    <musicbrainzalbumid>00000000-0000-0000-0000-000000000000</musicbrainzalbumid>
    <artistdesc>The Paula Channels</artistdesc>
    <genre>Synth-pop</genre>
    <genre>Chiptune</genre>
    <style>Chiptune</style>
    <style>Electronic</style>
    <mood>Warm</mood>
    <review>Their second album, and the one where the limits became the point.
Every song fits in 512K of chip memory &amp; the stereo is hard left and right.</review>
    <label>Copper Records</label>
    <type>album</type>
    <releasedate>1993-05-01</releasedate>
    <rating max="10">8.500000</rating>
    <thumb spoof="" cache="" aspect="thumb" preview="">folder.jpg</thumb>
    <track>
        <position>1</position>
        <title>Wait for the Beam</title>
        <duration>4:12</duration>
    </track>
    <albumArtistCredits>
        <artist>The Paula Channels</artist>
    </albumArtistCredits>
</album>
'''.encode('utf-8'))
mk(f'{root}/Artist/artist.nfo', '''<?xml version="1.0" encoding="UTF-8"?>
<artist>
  <name>The Paula Channels</name>
  <type>Group</type>
  <genre>Synth-pop</genre>
  <style>Chiptune</style>
  <formed>1988</formed>
  <disbanded>1996</disbanded>
  <biography><![CDATA[A four-piece from Lancaster.

They never used more than four channels <really>.]]></biography>
  <album>
    <title>Copper Lists</title>
    <year>1993</year>
  </album>
  <thumb aspect="thumb">folder.jpg</thumb>
</artist>
'''.encode('utf-8'))
mk(f'{root}/Artist/fanart.jpg', b'\xff\xd8')
mk(f'{root}/Artist/logo.png', b'\x89PNG')
mk(f'{root}/Artist/Album/discart.png', b'\x89PNG')
mk(f'{root}/Band/Live/CD1/01 a.mp3', tag + xing + b'\0' * 5000)
mk(f'{root}/Band/Live/CD2/01 b.mp3', tag + xing + b'\0' * 5000)
mk(f'{root}/Band/Live/album.nfo', '''<album><title>Live at the Café</title><year>1995</year>
<rating>4</rating><review>One night only.</review></album>'''.encode('utf-8'))
