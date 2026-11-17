"""A small beets library for tests/beets_68k.c: <dir>/music/musiclibrary.db
with beets' albums and items columns Ambiance reads, Windows paths as
beets on a PC writes them, and the files a few of them name in <dir>/music.

Usage: python3 -I mkbeets.py <dir>
"""
import os
import sqlite3
import sys

ROOT = "C:\\Users\\someone\\Music\\"
ALBUMS = [  # id, albumartist, album, year, tracks (title, file, format, artist)
    (1, "Various Artists", "Summer Hits", 2004, [
        ("Sun", "Various Artists\\Summer Hits\\01 Sun.mp3", "MP3", "Alpha"),
        ("Moon", "Various Artists\\Summer Hits\\02 Moon.wma", "Windows Media", "Beta"),
        ("Stars", "Various Artists\\Summer Hits\\03 Stars.mp3", "MP3", "Gamma"),
    ]),
    (2, "*NSYNC", "No Strings Attached", 2000, [
        ("Bye Bye Bye", "_NSYNC\\No Strings Attached\\01 Bye Bye Bye.mp3", "MP3", "*NSYNC"),
        ("It's Gonna Be Me", "_NSYNC\\No Strings Attached\\02 It's Gonna Be Me.flac", "FLAC",
         "*NSYNC"),
    ]),
    (3, "Beyoncé", "Dangerously in Love", 2003, [
        ("Crazy in Love", "Beyoncé\\Dangerously in Love\\01 Crazy in Love.mp3", "MP3", "Beyoncé"),
    ]),
    (4, "Βασίλης", "Ελληνικά", 1999, [
        ("Τραγούδι", "Βασίλης\\Ελληνικά\\01 Τραγούδι.mp3", "MP3", "Βασίλης"),
    ]),
    (5, "abba", "Arrival", 1976, [
        ("Dancing Queen", "ABBA\\Arrival\\02 Dancing Queen.mp3", "MP3", "ABBA"),
    ]),
    (6, "ABBA", "Waterloo", 1974, [
        ("Waterloo", "ABBA\\Waterloo\\01 Waterloo.mp3", "MP3", "ABBA"),
    ]),
]


def main():
    out = sys.argv[1]
    music = os.path.join(out, "music")
    os.makedirs(music, exist_ok=True)
    db = os.path.join(music, "musiclibrary.db")
    if os.path.exists(db):
        os.chmod(db, 0o644)
        os.remove(db)
    c = sqlite3.connect(db)
    c.execute("CREATE TABLE albums (id INTEGER PRIMARY KEY, artpath BLOB, added REAL, "
              "albumartist TEXT, album TEXT, year INTEGER)")
    c.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, path BLOB, album_id INTEGER, "
              "title TEXT, artist TEXT, track INTEGER, disc INTEGER, format TEXT)")
    c.execute("CREATE INDEX idx_item_album_id ON items (album_id)")
    for aid, artist, album, year, tracks in ALBUMS:
        c.execute("INSERT INTO albums VALUES (?, NULL, 1.5e9, ?, ?, ?)", (aid, artist, album, year))
        for n, (title, f, fmt, tartist) in enumerate(tracks, 1):
            c.execute("INSERT INTO items (path, album_id, title, artist, track, disc, format) "
                      "VALUES (?, ?, ?, ?, ?, 1, ?)",
                      ((ROOT + f).encode("utf-8"), aid, title, tartist, n, fmt))
            # the files there are on the Amiga: those Latin-1 can name
            try:
                f.encode("latin-1")
            except UnicodeEncodeError:
                continue
            p = os.path.join(music, *f.split("\\"))
            os.makedirs(os.path.dirname(p).encode("latin-1"), exist_ok=True)
            open(p.encode("latin-1"), "wb").close()
    c.commit()
    c.close()
    os.chmod(db, 0o444)


if __name__ == "__main__":
    main()
