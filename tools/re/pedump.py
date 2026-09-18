# re/pedump.py -- 按 VA 读原始 .game 的字节/双字(只读)
# 用法: pedump.py <image> <hexVA> <count> [d|b]
import struct, sys

def sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
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

def main():
    path, vahex, cnt = sys.argv[1], sys.argv[2], int(sys.argv[3])
    mode = sys.argv[4] if len(sys.argv) > 4 else "d"
    va = int(vahex, 16)
    data = open(path, "rb").read()
    imagebase, secs = sections(data)
    rva = va - imagebase
    off = None
    for name, vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + rawsize:
            off = rawptr + (rva - vaddr)
            break
    if off is None:
        print("VA not in any raw section"); return
    if mode == "d":
        for i in range(cnt):
            v = struct.unpack_from("<I", data, off + 4 * i)[0]
            f = struct.unpack_from("<f", data, off + 4 * i)[0]
            print("0x%08X  [%3d]  0x%08X  %12d   %g" % (va + 4 * i, i, v, v, f))
    else:
        for i in range(0, cnt, 16):
            chunk = data[off + i:off + i + 16]
            print("0x%08X  %s" % (va + i, " ".join("%02X" % b for b in chunk)))

main()
