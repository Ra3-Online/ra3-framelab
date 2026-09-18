# re/encl.py -- 反编译转储的只读查询小工具(不属于补丁本体)
# 2026-09-17 会话:动画问题定位。用途:
#   encl.py <行号...>            打印这些行落在哪个函数里
#   encl.py --addr 0x006027D0    打印该地址的函数定义行范围
#   encl.py --grep 正则          在函数定义行上做正则筛选
#   encl.py --fn sub_99BE00      打印该函数的函数体(直到下一个函数定义行)
#   encl.py --fn sub_A --fn sub_B 一次读多个
#
# 2026-09-17 12:40 追加 --fn:本会话要反复读整段函数体,每次手工找行范围太慢。
# 实现上直接复用 scan() 拿到的 (起始行, 地址, 名字) 列表,函数体 = 起始行..下一个起始行-1。
import io, re, sys

PATH = r"G:\IDA\RA3_1.12.game.c"
DEF = re.compile(r"^//----- \(00([0-9A-F]+)\) (.*?) -----")


def scan():
    funcs = []
    with io.open(PATH, "r", encoding="utf-8", errors="replace") as f:
        for i, l in enumerate(f, 1):
            m = DEF.match(l)
            if m:
                funcs.append((i, int(m.group(1), 16), m.group(2)))
    return funcs


def main():
    args = sys.argv[1:]
    funcs = scan()
    if not args:
        print("funcs=%d" % len(funcs))
        return
    if args[0] == "--addr":
        index = {a: (l, n) for l, a, n in funcs}
        order = sorted(index)
        import bisect
        for raw in args[1:]:
            # 2026-09-17:允许重复写 --addr(手滑写 `--addr A --addr B` 很自然),
            # 之前会把 '--addr' 当成十六进制数去解析然后崩掉。
            if raw == "--addr":
                continue
            want = int(raw, 16)
            if want in index:
                line, name = index[want]
                k = order.index(want)
                end = (order[k + 1] - 1) if k + 1 < len(order) else -1
                print("0x%08X %s  lines %d..%d" % (want, name, line, end))
            else:
                k = bisect.bisect_right(order, want) - 1
                if k < 0:
                    print("0x%08X  not found" % want)
                else:
                    line, name = index[order[k]]
                    print("0x%08X  inside 0x%08X %s (def line %d)" % (want, order[k], name, line))
        return
    if args[0] == "--fn":
        want = [a for a in args[1:] if a != "--fn"]
        byName = {n: (l, a) for l, a, n in funcs}
        starts = [f[0] for f in funcs]
        import bisect
        lines = io.open(PATH, "r", encoding="utf-8", errors="replace").read().splitlines()
        for w in want:
            if w not in byName:
                print("!! %s not found" % w)
                continue
            line, addr = byName[w]
            k = starts.index(line)
            end = (starts[k + 1] - 1) if k + 1 < len(starts) else len(lines)
            print("===== %s  0x%08X  lines %d..%d (%d lines) =====" % (w, addr, line, end, end - line + 1))
            for n in range(line, end + 1):
                print("%7d| %s" % (n, lines[n - 1]))
        return
    if args[0] == "--grep":
        rx = re.compile(args[1], re.I)
        for line, addr, name in funcs:
            if rx.search(name):
                print("%8d  0x%08X  %s" % (line, addr, name))
        return
    # enclosing
    starts = [f[0] for f in funcs]
    import bisect
    for a in args:
        n = int(a)
        k = bisect.bisect_right(starts, n) - 1
        line, addr, name = funcs[k]
        print("%8d -> 0x%08X %s (def line %d)" % (n, addr, name, line))


main()
