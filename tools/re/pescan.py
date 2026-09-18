# re/pescan.py -- 在原始 .game 里按字节模式扫描(只读,不属于补丁本体)
#
# 2026-09-17 动画定位会话重写。新增:
#   - 支持 `??` 通配(任意单字节),写模式时不用再为每个 ModRM 变体写一遍;
#   - 每个命中点自动解析出**所属函数名**(符号表复用 disasm.py 的缓存),
#     否则一堆裸地址根本读不动;
#   - `--fn` 只列函数名去重,`--all` 列全部命中。
#
# 用法:
#   pescan.py "G:\IDA\RA3_1.12.game" "D99?B0000000"          # fst/fstp dword [reg+0xB0]
#   pescan.py <image> "F30F11??B0000000" --fn               # movss [reg+0xB0],xmm
#
# 例(本会话用到的):找"谁写动画对象 +0xB0(当前帧)"。
import io, os, re, struct, sys

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_symcache.txt")


def load_symfuncs():
    """从 disasm.py 的符号缓存里读函数表 {va: name}。缓存不存在就返回空。"""
    fns = {}
    if os.path.exists(CACHE):
        for line in io.open(CACHE, "r", encoding="utf-8"):
            p = line.rstrip("\n").split("\t")
            if p[0] == "f":
                fns[int(p[1], 16)] = p[2]
    return fns


def enclosing(fns, va):
    if not fns:
        return "?"
    order = sorted(fns)
    import bisect
    k = bisect.bisect_right(order, va) - 1
    if k < 0:
        return "?"
    start = order[k]
    if k + 1 < len(order) and va >= order[k + 1]:
        return "?"
    return fns[start]


def load(path):
    data = open(path, "rb").read()
    e = struct.unpack_from("<I", data, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    optsize = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    ib = struct.unpack_from("<I", data, opt + 28)[0]
    sec = opt + optsize
    secs = []
    for i in range(nsec):
        o = sec + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, rp = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, va, vs, rp, rs))
    return data, ib, secs


def compile_pattern(hexpat):
    """把 'D99?B0000000' 编译成 (bytes, mask)。
    `??` = 整个字节任意;单个 `?` = 该**半字节**任意(比如 `9?` 匹配 0x90..0x9F)。
    mask 为 1 表示该字节必须整体匹配;半字节通配的字节记成 mask=2(交给逐字节校验里的 nibble 比较)。
    """
    hexpat = re.sub(r"[^0-9A-Fa-f?]", "", hexpat)
    if len(hexpat) % 2:
        raise SystemExit("模式长度必须是偶数个 hex 字符(现在 %d 个)" % len(hexpat))
    pat, mask, nib = bytearray(), bytearray(), []
    for i in range(0, len(hexpat), 2):
        c = hexpat[i:i + 2]
        if c == "??":
            pat.append(0)
            mask.append(0)
            nib.append((True, True))
        elif "?" in c:
            # c[0] 是**高**半字节、c[1] 是低半字节 —— 这里写反过一次,别再来
            hi_wild = c[0] == "?"
            lo_wild = c[1] == "?"
            hi = 0 if hi_wild else int(c[0], 16)
            lo = 0 if lo_wild else int(c[1], 16)
            pat.append((hi << 4) | lo)
            mask.append(2)
            nib.append((hi_wild, lo_wild))
        else:
            pat.append(int(c, 16))
            mask.append(1)
            nib.append((False, False))
    return bytes(pat), bytes(mask), nib


def find_all(data, pat, mask, nib, limit):
    """扫描。先用最长的一段"全确定"前缀做锚点快速定位,再逐字节校验(含半字节通配)。"""
    hits = []
    n = len(pat)
    if n == 0:
        return hits
    k = 0
    while k < n and mask[k] == 1:
        k += 1
    if k == 0:
        k = 1
    anchor = pat[:k]
    s = 0
    while len(hits) < limit:
        i = data.find(anchor, s)
        if i < 0:
            break
        ok = True
        for j in range(n):
            m = mask[j]
            if m == 1:
                if data[i + j] != pat[j]:
                    ok = False
                    break
            elif m == 2:
                hw, lw = nib[j]
                b = data[i + j]
                if not hw and (b >> 4) != (pat[j] >> 4):
                    ok = False
                    break
                if not lw and (b & 0xF) != (pat[j] & 0xF):
                    ok = False
                    break
        if ok:
            hits.append(i)
        s = i + 1
    return hits


def main():
    args = sys.argv[1:]
    fn_only = "--fn" in args
    args = [a for a in args if a != "--fn"]
    limit = 20000
    if "--limit" in args:
        i = args.index("--limit")
        limit = int(args[i + 1])
        del args[i:i + 2]
    path, hexpat = args[0], args[1]
    pat, mask, nib = compile_pattern(hexpat)
    data, ib, secs = load(path)

    def o2va(off):
        for name, va, vs, rp, rs in secs:
            if rp <= off < rp + rs:
                return ib + va + (off - rp), name
        return None, None

    hits = find_all(data, pat, mask, nib, limit)
    fns = load_symfuncs()
    print("pattern %s : %d hit(s)%s" % (hexpat, len(hits),
                                        "  (已截断)" if len(hits) >= limit else ""))
    seen = set()
    for h in hits:
        v, n = o2va(h)
        fname = enclosing(fns, v) if v else "?"
        if fn_only:
            if fname in seen:
                continue
            seen.add(fname)
            print("   %-14s VA 0x%08X  (%s)" % (fname, v, n))
        else:
            print("   file 0x%08X  VA 0x%08X  (%s)  %s" % (h, v, n, fname))


main()
