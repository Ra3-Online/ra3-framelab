#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
func_at.py -- 给「行号」找**宿主函数**（反编译 c 文件）。

为什么需要它：
  xref_by_func.py 回答「哪些函数引用了某符号」，但当我们在平表里看到一行
  反编译片段（例如 `005EB5E0  v11 = ... * (30.0 / dword_CAF9D4)`）时，真正要问的是
  「**这一行属于哪个函数**、它的地址是多少」——才能去看这个函数被谁调用、
  属于哪个子系统。手工翻几万行的 c 文件不现实。

用法:
  python func_at.py <反编译c文件> <行号> [<行号> ...]
  python func_at.py <反编译c文件> --find 0x005EB5E0      # 反查某地址的函数头行号

输出:
  行 414527  ->  //----- (00612340) sub_612340 -----  (头在行 413900, 函数体 413900..415200)
"""
import re
import sys

# IDA 反编译文件里每个函数的头部长这样：
#   //----- (00903910) sub_903910 -----
HDR = re.compile(rb'^//-+ \(([0-9A-Fa-f]{8})\) (\S+) -+', re.M)


def load_headers(path):
    """返回 [(hdr_line:int, addr:int, name:str)]，hdr_line 是 1 基行号。

    注意：行号必须**增量累计**。第一版对每个函数头都调
    data.count(b'\\n', 0, m.start())，在 100MB 级反编译文件上是 O(n^2)，
    直接被沙箱 SIGTERM 掉了（症状：无输出 + Exit 1 + SIGTERM）。
    """
    with open(path, 'rb') as f:
        data = f.read()
    out = []
    prev_end = 0
    nl = 0                      # 已数过的换行数（截至 prev_end）
    for m in HDR.finditer(data):
        nl += data.count(b'\n', prev_end, m.start())
        prev_end = m.start()
        out.append((nl + 1, int(m.group(1), 16), m.group(2).decode('ascii', 'replace')))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path = sys.argv[1]
    args = sys.argv[2:]
    hdrs = load_headers(path)
    if not hdrs:
        print('!! 没找到任何函数头，文件格式不对?')
        return 1

    if args[0] == '--find':
        want = int(args[1], 16)
        for line, addr, name in hdrs:
            if addr == want:
                print('0x%08X %s  ->  头在行 %d' % (addr, name, line))
                return 0
        print('!! 没找到 0x%08X' % want)
        return 1

    for a in args:
        ln = int(a)
        host = None
        for i, (line, addr, name) in enumerate(hdrs):
            if line <= ln:
                nxt = hdrs[i + 1][0] if i + 1 < len(hdrs) else 10 ** 9
                if ln < nxt:
                    host = (line, addr, name, nxt)
                    break
        if host is None:
            print('行 %d  ->  (在第一个函数头之前)' % ln)
        else:
            line, addr, name, nxt = host
            print('行 %-8d ->  0x%08X %-20s (头在行 %d, 函数体到行 %d)'
                  % (ln, addr, name, line, nxt - 1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
