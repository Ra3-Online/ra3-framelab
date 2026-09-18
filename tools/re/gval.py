# -*- coding: utf-8 -*-
# gval.py -- 直接读 PE 镜像里某个**全局变量**的**初始值**（不运行游戏）。
#
# 为什么需要它：判断一个全局是「编译期常量」还是「运行期被写」，
#   最快的证据就是先看它在文件里的初值。若初值 == 反编译里看到的那个常数
#   （例如 30），且整个反编译输出里没有任何 `sym = ...` 的赋值，
#   那它基本就是**静态名义值**，而不是「当前实测值」——
#   这决定了「把 X 当成当前帧率」的读者到底该不该重定向。
#
# 用法: python gval.py <镜像> <hexVA> [--n 8] [--fmt i|f]
#   --n   打印多少个 4 字节槽（默认 4，便于看相邻变量）
#   --fmt i 按 int 解释 / f 按 float 解释（默认两者都打）
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


def main():
    path = sys.argv[1]
    va = int(sys.argv[2], 16)
    n = 4
    a = sys.argv[3:]
    i = 0
    while i < len(a):
        if a[i] == '--n':
            n = int(a[i + 1]); i += 2
        else:
            i += 1

    d, secs = load(path)
    for k in range(n):
        addr = va + 4 * k
        hit = None
        for nm, sva, vs, rp, rs in secs:
            if sva <= addr < sva + vs:
                hit = (nm, rp + (addr - sva))
                break
        if hit is None:
            print("%08X  (不在任何节内)" % addr)
            continue
        nm, off = hit
        if off + 4 > len(d):
            print("%08X  %-8s (超出文件)" % (addr, nm))
            continue
        raw = d[off:off + 4]
        iv = struct.unpack_from("<i", raw)[0]
        uv = struct.unpack_from("<I", raw)[0]
        fv = struct.unpack_from("<f", raw)[0]
        print("%08X  %-8s  raw %s  int %-12d uint %-12u float %g"
              % (addr, nm, raw.hex(), iv, uv, fv))
    return 0


if __name__ == "__main__":
    main()
