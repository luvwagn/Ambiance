#!/usr/bin/env python3
"""expected.py: what jf_items() should read from each saved answer, by
Python's own JSON decoder, cut to struct JfItem's sizes as jfjson.c cuts
them; written to expected.txt beside this file (tests/jf_host.c compares)."""
import json, os
D = os.path.dirname(os.path.abspath(__file__))
F = [('name', 'Name', 160), ('id', 'Id', 36), ('type', 'Type', 24), ('coll', 'CollectionType', 16),
     ('media', 'MediaType', 12), ('album', 'Album', 120), ('album_id', 'AlbumId', 36),
     ('album_artist', 'AlbumArtist', 120), ('artist', None, 120), ('artist_id', None, 36),
     ('year', 'ProductionYear', 8), ('ticks', 'RunTimeTicks', 16), ('index', 'IndexNumber', 8),
     ('disc', 'ParentIndexNumber', 8), ('children', 'ChildCount', 8), ('container', 'Container', 16),
     ('image', None, 36), ('album_image', 'AlbumPrimaryImageTag', 36), ('width', 'Width', 8),
     ('height', 'Height', 8), ('date', 'PremiereDate', 24), ('rating', 'OfficialRating', 12),
     ('overview', 'Overview', 300)]

def cut(s, n):
    b = s.encode('utf-8')
    if len(b) >= n:
        k = n - 1
        while k and (b[k] & 0xC0) == 0x80:
            k -= 1
        b = b[:k]
    return b.decode('utf-8', 'replace').replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n').replace('\r', '\\r')

def text(v):
    if v is None or isinstance(v, (dict, list)) or isinstance(v, bool):
        return ''
    if isinstance(v, float) and v.is_integer() is False:
        return repr(v)
    return str(v)

out = []
for f in ['views', 'albums', 'artists', 'tracks', 'movies', 'photos']:
    d = json.load(open(os.path.join(D, f + '.json'), encoding='utf-8'))
    out.append('== %s %d %d' % (f, len(d['Items']), d.get('TotalRecordCount', len(d['Items']))))
    for it in d['Items']:
        row = []
        for key, src, n in F:
            if key == 'artist':
                v = (it.get('Artists') or [None])[0]
                if v is None and it.get('ArtistItems'):
                    v = it['ArtistItems'][0].get('Name')
            elif key == 'artist_id':
                v = (it.get('ArtistItems') or [{}])[0].get('Id')
            elif key == 'image':
                v = (it.get('ImageTags') or {}).get('Primary')
            else:
                v = it.get(src)
            row.append(cut(text(v), n))
        row.append('1' if it.get('IsFolder') is True else '0')
        out.append('\t'.join(row))
open(os.path.join(D, 'expected.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print('expected.txt:', len(out), 'lines')
