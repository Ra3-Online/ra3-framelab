# tools/re/refs.py -- list every instruction that references an absolute address, by function.
# 2026-09-26: written for the scripted-camera census (all readers of 0x00CE176C).
# Decodes each containing function LINEARLY from its start (function starts come from the IDA dump).
# Do not decode backwards from the operand: that mis-aligns (e.g. 'F7 3D' idiv reads as 'cmp eax,imm').
# Usage: refs.py <hexAddr> [funcName,funcName,...]
import struct,re,bisect,sys
from capstone import *
from local_config import image_path, ida_dump_path, take_input_options
args, options = take_input_options(sys.argv[1:])
if not args:
    raise SystemExit('usage: refs.py <hexAddr> [funcName,...] [--image PATH] [--ida-dump PATH]')
d=image_path(options.get('--image')).read_bytes()
pe=struct.unpack_from('<I',d,0x3c)[0];ns=struct.unpack_from('<H',d,pe+6)[0];so=struct.unpack_from('<H',d,pe+20)[0];sec=pe+24+so;ib=struct.unpack_from('<I',d,pe+24+28)[0]
secs=[]
for k in range(ns):
    b=sec+40*k;name=d[b:b+8].rstrip(b'\0');vs,va,rs,ra=struct.unpack_from('<IIII',d,b+8);secs.append((name,ib+va,rs,ra))
T=[s for s in secs if s[0]==b'.text'][0]
def va2off(v): return T[3]+v-T[1]
starts=[];names={}
for line in open(ida_dump_path(options.get('--ida-dump'), required=True),encoding='utf-8',errors='replace'):
    m=re.match(r'^//----- \(00([0-9A-F]+)\) (\S+) -----',line)
    if m: v=int(m.group(1),16);starts.append(v);names[v]=m.group(2)
starts.sort()
md=Cs(CS_ARCH_X86,CS_MODE_32)
tgt=int(args[0],16); only=set(args[1].split(',')) if len(args)>1 else None
p=struct.pack('<I',tgt); o=T[3]; end=o+T[2]; fns=set(); i=o
while True:
    i=d.find(p,i,end)
    if i<0: break
    va=T[1]+(i-o); k=bisect.bisect_right(starts,va)-1; fns.add(starts[k]); i+=1
for f in sorted(fns):
    if only and names[f] not in only: continue
    k=starts.index(f); fend=starts[k+1] if k+1<len(starts) else f+0x1000
    for ins in md.disasm(d[va2off(f):va2off(fend)],f):
        if ('0x%x'%tgt) in ins.op_str.lower():
            print('%08X %-11s %-38s [%s]'%(ins.address,names[f],ins.mnemonic+' '+ins.op_str,ins.bytes.hex(' ')))
