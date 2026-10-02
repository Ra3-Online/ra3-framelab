#!/usr/bin/env python3
# tools/re/xref.py -- 穷尽枚举「哪些指令引用了某个绝对地址」。
#
# 2026-09-17 动画定位会话新增。起因是一次**真错**:
#   我扫 `A3 88 13 CE 00`(`mov [0x00CE1388],eax`)找时钟的写入点,得到 2 处,
#   但用 `| head -10` 截断输出只看见第一行,于是写下「时钟只有一个写入点」,
#   并据此只改了一条分支 —— 留下约 5.6% 的偏快(量化记录原件已另行归档；机制摘要见 docs/TECHNICAL.md)。
#   pescan.py 只能按**你猜的那种编码形式**扫;猜漏一种形式(比如
#   `mov edx,[addr]` 是 `8B 15` 而不是 `A1`)就永远找不到。
#
# 这个工具换个方向:**把 .text 整段线性反汇编一遍**,然后筛出所有操作数里
# 含目标地址的指令。不预设编码形式,所以 `mov [addr],eax` / `mov [addr],ecx` /
# `mov edx,[addr]` / `add [addr],imm` / `inc [addr]` / `mov dword [addr],imm32` … 全都抓到。
#
# ⚠ 为什么必须整段反汇编而不是「从字面量往前回退」:
#   初版就是回退法,结果 `back=0` 永远能解出一条指令(把 `88 13` 当成
#   `mov [ebx],dl`),输出全是垃圾。回退法无法判断对齐,**不可用**。
#
# 局限(必须知道):
#   · 线性反汇编在「.text 里嵌了数据」(跳表等)处会失步。脚本会报**覆盖率**;
#     低于 98% 就说明失步较多,结论要打折。MSVC 一般把跳表放 .rdata,所以通常很干净。
#   · 只覆盖**绝对地址**形式。经寄存器间接寻址(`mov eax,[reg+off]`)抓不到。
#
# 用法:
#   xref.py <image> <hexVA> [hexVA...]
#   xref.py "path/to/ra3_1.12.game" 0x00CE1388 0x00CE176C

import struct
import sys

try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
except ImportError:
    sys.exit("需要 capstone: python -m pip install capstone")


def load(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[e:e + 4] == b"PE\0\0", "不是 PE 文件"
    c = e + 4
    ns = struct.unpack_from("<H", d, c + 2)[0]
    osz = struct.unpack_from("<H", d, c + 16)[0]
    opt = c + 20
    ib = struct.unpack_from("<I", d, opt + 28)[0]
    secs = []
    so = opt + osz
    for i in range(ns):
        o = so + i * 40
        nm = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, rp = struct.unpack_from("<IIII", d, o + 8)
        secs.append((nm, va, vs, rp, rs))
    return d, ib, secs


def sweep(data, ib, va0, rp, rs, md):
    """线性反汇编一个段(带失步重同步),返回 (指令列表, 覆盖率)。

    指令列表元素: (va, mnemonic, op_str)。
    覆盖率 = 被成功解码的字节 / 段长度。失步多说明段里有数据,结论要打折。

    ⚠ 为什么要重同步:`md.disasm` 遇到第一个无效字节就**停住**(实测直接跑
    `md.disasm(整段)` 只覆盖了 0.64% 就没了)。所以分块解码,块内解不动就
    往前挪一个字节再试 —— 这样即使中间嵌了数据/跳表也能继续。
    """
    code = data[rp:rp + rs]
    n = len(code)
    insns = []
    covered = 0
    off = 0
    CHUNK = 4096
    while off < n:
        progressed = False
        for ins in md.disasm(code[off:off + CHUNK], ib + va0 + off):
            insns.append((ins.address, ins.mnemonic, ins.op_str))
            covered += ins.size
            off += ins.size
            progressed = True
        if not progressed:
            off += 1          # 失步:跳一字节重新对齐
    return insns, covered / float(n) if n else 0.0


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, targets = sys.argv[1], [int(a, 16) for a in sys.argv[2:]]
    data, ib, secs = load(path)
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    # 只反汇编 .text(代码)。数据段里的字面量不是指令。
    for nm, va0, vs, rp, rs in secs:
        if nm != ".text":
            continue
        insns, cov = sweep(data, ib, va0, rp, rs, md)
        print("反汇编 .text:%d 字节 → %d 条指令,覆盖率 %.2f%%"
              % (rs, len(insns), cov * 100.0))
        if cov < 0.98:
            print("⚠ 覆盖率偏低 ⇒ 段里有数据/失步,下面的结论要打折")

        for tva in targets:
            # 目标地址以十六进制出现在操作数字符串里(capstone 用小写 hex,如 0xce1388)
            needle = "0x%x" % tva
            print("=" * 78)
            print("地址 0x%08X 的引用:" % tva)
            n = 0
            for addr, mn, ops in insns:
                if needle not in ops:
                    continue
                # 判读读写:x86 Intel 语法是 `指令 目的, 源`。
                # 目标出现在逗号**之前** ⇒ 它是目的 ⇒ 写;之后 ⇒ 读。
                pos, comma = ops.find(needle), ops.find(",")
                if comma < 0:
                    tag = "立即数/单操作数"
                elif pos < comma:
                    tag = "★写"
                else:
                    tag = "读"
                print("   %08X  %-8s %-46s %s" % (addr, mn, ops, tag))
                n += 1
            if n == 0:
                print("   (无 —— 该地址可能只经寄存器间接寻址访问)")
        return


if __name__ == "__main__":
    main()
