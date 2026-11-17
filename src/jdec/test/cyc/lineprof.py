#!/usr/bin/env python3
"""Per-line and per-function profile of jdec on the 68020 (Musashi PC sampling).
AmigaMCE, MIT.

    python3 lineprof.py <cyc.elf built with -g> <cyc.bin> <file.jpg> <scales, e.g. 0,3> [lines]

Run from this directory (it imports cyc.py)."""
import sys, subprocess, re
from collections import Counter
sys.path.insert(0,'.')
import machine68k, cyc
elf,binf,jpg=sys.argv[1:4]; scales=[int(x) for x in sys.argv[4].split(',')]; top=int(sys.argv[5]) if len(sys.argv)>5 else 14
binary=open(binf,'rb').read(); data=open(jpg,'rb').read()
src={'jdec.c':open('../../jdec.c').read().split('\n'),'cyc.c':open('cyc.c').read().split('\n')}
entry=int([l.split()[0] for l in subprocess.run(['m68k-linux-gnu-nm',elf],capture_output=True,text=True).stdout.splitlines() if l.endswith(' start')][0],16)
for scale in scales:
    m=machine68k.Machine(machine68k.cpu_type_from_str('68020'),16*1024); mem,cpu=m.mem,m.cpu
    mem.w32(0,cyc.STACK); mem.w32(4,entry); cpu.pulse_reset()
    end=m.create_execute_end('d'); tid=m.traps.alloc(lambda o,p: end); mem.w16(cyc.RET,0xA000|tid)
    mem.w_block(cyc.CODE,binary); mem.w_block(cyc.JPEG,data)
    for i,v in enumerate((cyc.JPEG,len(data),scale,0,cyc.HEAP)): mem.w32(cyc.PARAMS+4*i,v)
    cpu.w_sp(cyc.STACK-4); mem.w32(cyc.STACK-4,cyc.RET); cpu.w_pc(entry)
    h=Counter()
    while True:
        r=m.execute(331)
        if r.result is end: break
        h[cpu.r_pc()]+=1
    n=sum(h.values()); addrs=list(h.keys())
    out=subprocess.run(['m68k-linux-gnu-addr2line','-f','-e',elf]+['%x'%a for a in addrs],capture_output=True,text=True).stdout.split('\n')
    lines=Counter(); funcs=Counter()
    for i,a in enumerate(addrs):
        fn=out[2*i]; l=re.sub(r' \(discriminator \d+\)','',out[2*i+1]).split('/')[-1]; lines[l]+=h[a]; funcs[fn]+=h[a]
    print('scale 1/%d:  '%(1<<scale)+', '.join('%s %.1f%%'%(f,100*c/n) for f,c in funcs.most_common(6)))
    for l,c in lines.most_common(top):
        f,_,ln=l.partition(':'); txt=src.get(f,[''])[int(ln)-1].strip() if ln.isdigit() else ''
        print('  %5.1f%% %-12s %s'%(100*c/n,l,txt[:72]))
