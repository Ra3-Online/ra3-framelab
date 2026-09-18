# -*- coding: utf-8 -*-
# bytes_at.py -- dump PE 镜像里某个 VA 起的一段字节（hex + ascii + 作为指针的解释）。
#
# 为什么需要它：读全局时经常看到「int 15 / int 30」后面紧跟几个 .rdata 指针，
# 那往往是一个**配置项结构体**（数值 + 名字字符串指针）。把指针跟进去看字符串，
# 就能给这两个数值定性 —— 是「玩家可设的帧率上下限」还是「别的什么东西」。
#
# 用法: python bytes_at.py <镜像> <hexVA> [--n 96] [--follow]
#   --follow  把每个像 .rdata 指针的 4 字节跟进去，也 dump 目标处的字符串
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    ib = struct.unpack_from("<I", d, opt + 28)[0]
    secs = []
    for i in range(nsec):
        o = opt + optsz + i * 40
        nm = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, rp = struct.unpack_from("<IIII", d, o + 8)
        secs.append((nm, ib + va, vs, rp, rs))
    return d, secs


def off_of(secs, va):
    for nm, sva, vs, rp, rs in secs:
        if sva <= va < sva + vs:
            return rp + (va - sva)
    return None


def cstr(d, off, maxlen=80):
    end = d.find(b'\0', off)
    if end < 0 or end - off > maxlen:
        end = off + maxlen
    raw = d[off:end]
    try:
        return raw.decode('ascii')
    except UnicodeDecodeError:
        return repr(raw)


def main():
    path = sys.argv[1]
    va = int(sys.argv[2], 16)
    n = 96
    follow = False
    a = sys.argv[3:]
    i = 0
    while i < len(a):
        if a[i] == '--n':
            n = int(a[i + 1]); i += 2
        elif a[i] == '--follow':
            follow = True; i += 1
        else:
            i += 1

    d, secs = load(path)
    print("VA %08X 起 %d 字节:" % (va, n))
    for k in range(0, n, 16):
        addr = va + k
        o = off_of(secs, addr)
        if o is None:
            print("  %08X  (越界)" % addr)
            break
        chunk = d[o:o + 16]
        hx = ' '.join('%02X' % b for b in chunk)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print("  %08X  %-47s  %s" % (addr, hx, asc))

    if follow:
        print("--- 把每个 4 字节当指针跟进 ---")
        for k in range(0, n, 4):
            addr = va + k
            o = off_of(secs, addr)
            if o is None:
                break
            v = struct.unpack_from("<I", d, o)[0]
            t = off_of(secs, v)
            if t is None or v < 0x00400000:
                continue
            print("  [%08X] -> %08X : %s" % (addr, v, cstr(d, t)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
