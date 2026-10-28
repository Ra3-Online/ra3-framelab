"""Read-only inventory of direct engine-alpha readers in the supplied retail PE.

Requires capstone. Optional IDA text supplies enclosing function boundaries.
This covers the explicit TheGameEngine load followed by a field +0x60 read;
it does not claim that aliased pointers or all animation paths are exhausted.
"""
import argparse,bisect,hashlib,json,re,struct
from pathlib import Path
from capstone import Cs,CS_ARCH_X86,CS_MODE_32,CS_AC_READ
from capstone.x86 import X86_OP_IMM,X86_OP_MEM,X86_OP_REG
from disasm import Image

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image',type=Path)
    parser.add_argument('--ida-dump',type=Path)
    args=parser.parse_args()
    image=Image(args.image)
    section=next(s for s in image.secs if s[0]=='.text')
    _,va,_,raw,size=section
    text=image.data[raw:raw+size]
    start=image.imagebase+va
    engine=struct.pack('<I',0xCE2F8C)
    functions=[]
    if args.ida_dump:
        with args.ida_dump.open(encoding='utf-8',errors='replace') as stream:
            for line in stream:
                match=re.match(r'^//----- \(([0-9A-F]+)\)',line)
                if match: functions.append(int(match[1],16))
    functions=sorted(set(functions))
    decoder=Cs(CS_ARCH_X86,CS_MODE_32);decoder.detail=True
    loads=[];readers={};limited=0
    alias={'al':'eax','ah':'eax','ax':'eax','cl':'ecx','ch':'ecx','cx':'ecx',
           'dl':'edx','dh':'edx','dx':'edx','bl':'ebx','bh':'ebx','bx':'ebx',
           'si':'esi','di':'edi','sp':'esp','bp':'ebp'}
    def register(reg):
        name=decoder.reg_name(reg)
        return alias.get(name,name)
    for offset in range(0,len(text)-6):
        if text[offset:offset+1]==b'\xa1' and text[offset+1:offset+5]==engine:
            length=5
        elif text[offset:offset+1]==b'\x8b' and (text[offset+1]&0xC7)==5 and text[offset+2:offset+6]==engine:
            length=6
        else: continue
        instructions=list(decoder.disasm(text[offset:offset+96],start+offset))
        if not instructions or instructions[0].size!=length: continue
        load=instructions[0]; reg=register(load.operands[0].reg)
        loads.append(load.address)
        fi=bisect.bisect_right(functions,load.address)-1
        lower=functions[fi] if fi>=0 else load.address
        upper=functions[fi+1] if 0<=fi<len(functions)-1 else min(start+len(text),load.address+96)
        pending=[(load.address+load.size,frozenset([reg]))];visited=set()
        while pending and len(visited)<4096:
            pc,known=pending.pop()
            if not known or (pc,known) in visited or not lower<=pc<upper: continue
            visited.add((pc,known))
            instruction=next(decoder.disasm(image.read(pc,min(15,upper-pc)),pc,count=1),None)
            if instruction is None: continue
            for operand in instruction.operands:
                if operand.type==X86_OP_MEM and register(operand.mem.base) in known and not operand.mem.index and operand.mem.disp==0x60 and operand.access&CS_AC_READ:
                    readers[instruction.address]={'load':f'{load.address:08X}','reader':f'{instruction.address:08X}',
                        'function':f'{lower:08X}' if fi>=0 else None,
                        'bytes':instruction.bytes.hex(' '),'instruction':instruction.mnemonic+' '+instruction.op_str}
            updated=set(known)
            _,writes=instruction.regs_access()
            updated.difference_update(register(r) for r in writes)
            ops=instruction.operands
            if instruction.mnemonic=='mov' and len(ops)==2 and ops[0].type==X86_OP_REG:
                source=ops[1]
                if ((source.type==X86_OP_REG and register(source.reg) in known) or
                    (source.type==X86_OP_MEM and not source.mem.base and source.mem.disp==0xCE2F8C)):
                    updated.add(register(ops[0].reg))
            if instruction.mnemonic.startswith('ret'): continue
            if instruction.mnemonic=='call': updated.difference_update(['eax','ecx','edx'])
            successor=instruction.address+instruction.size
            if instruction.mnemonic.startswith('j'):
                if ops and ops[0].type==X86_OP_IMM: pending.append((ops[0].imm,frozenset(updated)))
                if instruction.mnemonic=='jmp': continue
            pending.append((successor,frozenset(updated)))
        limited+=bool(pending)
    print(json.dumps({'image_sha256':hashlib.sha256(image.data).hexdigest(),
        'direct_engine_loads':len(loads),'direct_alpha_readers':[readers[k] for k in sorted(readers)],'game_executed':False,
        'limited_walks':limited,'scope':'Explicit engine loads, register copies and intra-function branches; calls kill volatile registers; stack/heap aliases and indirect callees excluded'},indent=2))
if __name__=='__main__': main()
