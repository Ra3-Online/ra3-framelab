# -*- coding: utf-8 -*-
# find_vt.py -- 在 PE 镜像的**数据节**里找一个 4 字节值（通常是函数 VA），
#              用来判断某个函数是否被某张 vtable 收了。
#
# 为什么需要它：IDA 的反编译输出里，虚函数的调用点长成
#     (*(...)(*(_DWORD *)obj + 偏移))(obj)
# 反编译器不会告诉我们「偏移 116 对应哪个函数」——那个对应关系只在 .rdata 的
# vtable 里。所以想弄清「谁调用了 sub_4FCC00」，就得去数据节里找 00 CC 4F 00。
#
# 用法: python find_vt.py <镜像> <hexVA> [<hexVA> ...]
import struct
import sys


def load_sections(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    # ★ 镜像基址必须加上：节表里的 VirtualAddress 是 RVA，不是 VA。
    #   上一轮就是漏了这一步，导致 off(VA) 返回 None。
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
    targets = [int(x, 16) for x in sys.argv[2:]]
    d, secs = load_sections(path)
    for nm, va, vs, rp, rs in secs:
        print("sec %-8s VA %08X vsize %8X raw %8X rsize %8X" % (nm, va, vs, rp, rs))
    for t in targets:
        print("--- 值 %08X 在数据里的出现位置 ---" % t)
        n = 0
        for nm, va, vs, rp, rs in secs:
            blob = d[rp:rp + rs]
            for i in range(0, len(blob) - 3, 4):
                if struct.unpack_from("<I", blob, i)[0] == t:
                    print("  %-8s VA %08X" % (nm, va + i))
                    n += 1
        if n == 0:
            print("  （未找到）")


if __name__ == "__main__":
    main()
