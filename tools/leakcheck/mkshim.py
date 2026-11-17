#!/usr/bin/env python3
"""make leakcheck: the NDK's acquire/release pairs as clang's ownership
attributes, so that its static analyser (unix.Malloc, optimistic) reports
whatever is acquired on a path that never releases it or passes it on.

Writes into OUT (build/leakcheck):
  shim.h            forced into every file: the NDK prototypes (no inline
                    calls) redeclared with ownership_returns/ownership_takes
  ov/dos/dos.h      the NDK's, BPTR made a pointer (Open/Lock tracked too)
  ov/inline/*.h     the NDK's inline calls replaced: macros.h's LPn as
                    opaque calls through the base, the rest empty (their
                    prototypes come from clib/)
Usage: mkshim.py NDK_INCLUDE OUT
"""
import glob
import os
import re
import sys

ndk, out = sys.argv[1], sys.argv[2]

# kind: (acquirers, releaser, index of the released argument)
PAIRS = [
    ('AllocVec', 'FreeVec', 1), ('AllocMem', 'FreeMem', 1), ('Open', 'Close', 1),
    ('Lock DupLock ParentDir DupLockFromFH ParentOfFH CreateDir', 'UnLock', 1),
    ('AllocDosObject AllocDosObjectTagList AllocDosObjectTags', 'FreeDosObject', 2),
    ('CreateMsgPort', 'DeleteMsgPort', 1), ('CreateIORequest', 'DeleteIORequest', 1),
    ('AllocBitMap', 'FreeBitMap', 1), ('OpenFont OpenDiskFont', 'CloseFont', 1),
    ('OpenLibrary', 'CloseLibrary', 1), ('CreatePool', 'DeletePool', 1),
    ('NewRegion', 'DisposeRegion', 1), ('GetDiskObject GetDiskObjectNew', 'FreeDiskObject', 1),
    ('NewDTObjectA NewDTObject', 'DisposeDTObject', 1), ('LockPubScreen', 'UnlockPubScreen', 2),
    ('OpenWindowTagList OpenWindowTags OpenWindow', 'CloseWindow', 1),
    ('OpenScreenTagList OpenScreenTags OpenScreen', 'CloseScreen', 1),
    ('AllocScreenBuffer', 'FreeScreenBuffer', 2),
    ('AllocAslRequest AllocAslRequestTags', 'FreeAslRequest', 1), ('ReadArgs', 'FreeArgs', 1),
    ('GetVisualInfoA GetVisualInfo', 'FreeVisualInfo', 1), ('CreateMenusA CreateMenus', 'FreeMenus', 1),
    ('AddAppIconA AddAppIcon', 'RemoveAppIcon', 1), ('OpenLocale', 'CloseLocale', 1),
]

protos = {}
for f in glob.glob(os.path.join(ndk, 'clib', '*_protos.h')):
    for line in open(f, encoding='latin-1'):
        m = re.match(r'^(.*?)\b__stdargs\s+(\w+)\s*\((.*)\);', line.strip())
        if m:
            protos[m.group(2)] = (m.group(1).strip(), m.group(3))

decl = []
for acq, rel, n in PAIRS:
    # (clang models the kind "malloc" only: every pair is one kind)
    for a in acq.split():
        if a in protos:
            r, args = protos[a]
            decl.append(f'{r} {a}({args}) __attribute__((ownership_returns(malloc)));')
    if rel in protos:
        r, args = protos[rel]
        decl.append(f'{r} {rel}({args}) __attribute__((ownership_takes(malloc, {n})));')

os.makedirs(os.path.join(out, 'ov', 'dos'), exist_ok=True)
os.makedirs(os.path.join(out, 'ov', 'inline'), exist_ok=True)
with open(os.path.join(out, 'shim.h'), 'w') as f:
    f.write('/* make leakcheck (tools/leakcheck/mkshim.py): generated */\n'
            '#define _NO_INLINE 1\n#define __stdargs\n#define __saveds\n#define __chip\n'
            '#define __far\n#define __asm(x)\n#define __aligned __attribute__((aligned(4)))\n'
            '#define AMB_VERSION "leakcheck"\n#define AMB_DATE "leakcheck"\n'
            '#include <exec/types.h>\n')
    for lib in ('exec', 'dos', 'graphics', 'intuition', 'diskfont', 'datatypes', 'asl', 'icon',
                'alib', 'gadtools', 'wb', 'locale'):
        f.write(f'#include <clib/{lib}_protos.h>\n')
    f.write('\n'.join(decl) + '\n')

dos = open(os.path.join(ndk, 'dos', 'dos.h'), encoding='latin-1').read()
open(os.path.join(out, 'ov', 'dos', 'dos.h'), 'w', encoding='latin-1').write(
    dos.replace('typedef long BPTR;', 'typedef struct BPTR_s *BPTR;'))

macros = open(os.path.join(ndk, 'inline', 'macros.h'), encoding='latin-1').read()
lines = ['#ifndef __INLINE_MACROS_H', '#define __INLINE_MACROS_H']
for m in re.finditer(r'#define (LP\w+)\(([^)]*)\)', macros):
    name, params = m.group(1), [p.strip() for p in m.group(2).split(',')]
    ts = [p for p in params if re.fullmatch(r't\d+', p)]
    vs = ['v' + t[1:] for t in ts]
    pre = 'typedef fpt; ' if 'fpt' in params else ''
    rt = 'rt' if 'rt' in params else 'void'
    lines.append(f'#define {name}({",".join(params)}) '
                 f'({{ {pre}(({rt}(*)({",".join(ts) or "void"}))(void*)(bn))({",".join(vs)}); }})')
lines.append('#endif')
open(os.path.join(out, 'ov', 'inline', 'macros.h'), 'w').write('\n'.join(lines) + '\n')
for f in glob.glob(os.path.join(ndk, 'inline', '*.h')):
    b = os.path.basename(f)
    if b not in ('macros.h', 'stubs.h'):
        open(os.path.join(out, 'ov', 'inline', b), 'w').write('/* make leakcheck: prototypes */\n')
