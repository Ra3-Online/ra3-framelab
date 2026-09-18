#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
xref_by_func.py -- 按**函数**归并的交叉引用普查。

为什么需要它（而不是 tools/re/xref.py）：
  xref.py 给的是「某个地址被哪些指令引用」的**扁平原表**。但排查「建筑动画为什么坏」
  时真正要问的是**另一个方向的问题**：
      「哪些函数在读帧率/时钟这几个全局？它们各自在哪一段代码里？」
  也就是「谁在消费」，而不是「谁被引用」。
  按函数归并之后，一眼就能看出某一段区间（比如 W3D/动画区间 0x8E0000-0x940000）
  里有几个读者、分别是谁，从而判断「我改的地方是不是唯一的那条时间路径」。

用法:
  python xref_by_func.py <反编译c文件> <hexVA> [<hexVA> ...]
  python xref_by_func.py <反编译c文件> --range 0x8E0000 0x940000 <hexVA> ...
选项:
  --range LO HI   只打印函数地址落在 [LO, HI) 区间内的函数
  --top N         每个符号最多打印 N 个函数（按引用次数降序），默认全部

输出示例:
  008EA220  sub_8EA220       3 次   dword_CAF9D4
  ...
"""
import re
import sys

# IDA 反编译文件里每个函数的头部长这样：
#   //----- (00903910) sub_903910 -----
# 我们靠它切分函数。
HDR = re.compile(rb'^//-+ \(([0-9A-Fa-f]{8})\) (\S+) -+', re.M)


def split_functions(path):
    """把文件切成 [(addr:int, name:str, body:bytes), ...]（按出现顺序）。"""
    with open(path, 'rb') as f:
        data = f.read()
    ms = list(HDR.finditer(data))
    out = []
    for i, m in enumerate(ms):
        addr = int(m.group(1), 16)
        name = m.group(2).decode('ascii', 'replace')
        start = m.end()
        end = ms[i + 1].start() if i + 1 < len(ms) else len(data)
        out.append((addr, name, data[start:end]))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path = sys.argv[1]
    args = sys.argv[2:]

    lo = hi = None
    top = None
    syms = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--range':
            lo = int(args[i + 1], 16)
            hi = int(args[i + 2], 16)
            i += 3
        elif a == '--top':
            top = int(args[i + 1])
            i += 2
        else:
            syms.append(a)
            i += 1

    funcs = split_functions(path)
    print('# 函数总数 %d' % len(funcs))
    if lo is not None:
        print('# 区间过滤: [%08X, %08X)' % (lo, hi))

    for sym in syms:
        pat = sym.encode('ascii')
        hits = []
        for addr, name, body in funcs:
            n = body.count(pat)
            if not n:
                continue
            if lo is not None and not (lo <= addr < hi):
                continue
            hits.append((addr, name, n))
        hits.sort(key=lambda t: (-t[2], t[0]))
        total = sum(h[2] for h in hits)
        print('\n=== %s : %d 处引用 / %d 个函数 ===' % (sym, total, len(hits)))
        shown = hits if top is None else hits[:top]
        for addr, name, n in shown:
            print('  %08X  %-24s %d 次' % (addr, name, n))
        if top is not None and len(hits) > top:
            print('  ... 还有 %d 个函数未列出' % (len(hits) - top))
    return 0


if __name__ == '__main__':
    sys.exit(main())
