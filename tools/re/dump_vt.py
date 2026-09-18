# -*- coding: utf-8 -*-
# dump_vt.py -- 从「vtable 里某个已知条目地址」出发，向两边展开 dump，
#               并把每个条目标注为 <code>(落在 .text 里) / 其它。
#
# 为什么需要它：find_vt.py 告诉我们「某个函数被哪张 vtable 收了」，
# 但要判断**这是哪个类**、类有多大、某个槽位对应哪个函数，就得把整张表摊开看。
# vtable 在 .rdata 里是连续的 dd 函数指针，边界靠「不再指向 .text」来判定。
#
# 用法: python dump_vt.py <镜像> <hexVA> [--back N] [--fwd N]
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


def off_of(d, secs, va):
    for nm, sva, vs, rp, rs in secs:
        if sva <= va < sva + vs:
            return rp + (va - sva)
    return None


def main():
    path = sys.argv[1]
    va = int(sys.argv[2], 16)
    back = 40
    fwd = 120
    a = sys.argv[3:]
    i = 0
    while i < len(a):
        if a[i] == '--back':
            back = int(a[i + 1]); i += 2
        elif a[i] == '--fwd':
            fwd = int(a[i + 1]); i += 2
        else:
            i += 1

    d, secs = load(path)
    text = next(s for s in secs if s[0] == '.text')
    tlo, thi = text[1], text[1] + text[2]

    def is_code(v):
        return tlo <= v < thi

    def rd(addr):
        o = off_of(d, secs, addr)
        if o is None:
            return None
        return struct.unpack_from("<I", d, o)[0]

    print("目标条目 VA %08X" % va)
    print("--- 向前 (槽号递减) ---")
    for k in range(1, back + 1):
        addr = va - k * 4
        v = rd(addr)
        if v is None:
            break
        if not is_code(v):
            break
        print("  [%+4d B] %08X : %08X  code" % (-k * 4, addr, v))
    print("--- 目标及其后 (槽号递增) ---")
    for k in range(0, fwd + 1):
        addr = va + k * 4
        v = rd(addr)
        if v is None:
            print("  [%+4d B] %08X : (越界)" % (k * 4, addr))
            break
        tag = "code" if is_code(v) else "非代码/结束"
        print("  [%+4d B] %08X : %08X  %s" % (k * 4, addr, v, tag))
        if not is_code(v):
            break


if __name__ == "__main__":
    main()
