# re/pefind.py -- 在原始 .game 二进制里找「谁引用了某个 VA」
# 2026-09-17 会话:动画问题定位。反编译转储里看不到 vtable 引用,只能回二进制扫指针。
# 用法: pefind.py <image> <hexVA> [maxhits]
#   例: pefind.py "path/to/ra3_1.12.game" 0x005B81A0
import struct, sys

def sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0"
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    optsize = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    imagebase = struct.unpack_from("<I", data, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return imagebase, out

def va_to_off(secs, imagebase, va):
    rva = va - imagebase
    for name, vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr), name
    return None, None

def off_to_va(secs, imagebase, off):
    for name, vaddr, vsize, rawptr, rawsize in secs:
        if rawptr <= off < rawptr + rawsize:
            return imagebase + vaddr + (off - rawptr), name
    return None, None

def main():
    path, vahex = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    va = int(vahex, 16)
    data = open(path, "rb").read()
    imagebase, secs = sections(data)
    pat = struct.pack("<I", va)
    hits = []
    start = 0
    while True:
        i = data.find(pat, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    print("%s: imagebase=0x%08X, hits=%d" % (path, imagebase, len(hits)))
    for off in hits[:limit]:
        hva, sec = off_to_va(secs, imagebase, off)
        print("  file 0x%08X  VA 0x%08X  (%s)" % (off, hva, sec))
    if len(hits) > limit:
        print("  ... %d more" % (len(hits) - limit))

main()
