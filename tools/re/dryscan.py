#!/usr/bin/env python3
# tools/re/dryscan.py -- 离线干扫:不开游戏,就验证 framelab.cpp 里每一条特征码能不能定位。
#
# 2026-09-17 动画定位会话新增。为什么需要它:
#   游戏内 `flctl dryrun` 能验证同样的东西,但它要先起游戏、等 45 秒、注入 —— 一轮两分钟,
#   而且改一次特征码就得重来。这个脚本把同一套判据搬到离线:直接读原始 .game,按 .text 段
#   扫特征,并和源码里写的期望地址(expectedVa)对账。改完特征码先跑它,通过了再起游戏。
#
# ★它已经抓到过一次真 bug:kSigClockAdvance 的第一版签名漏掉了中间的 `push eax` 一个字节,
#   离线干扫报 0 命中。要是直接起游戏,那一轮就白跑了(而且 flctl 只会给个「特征没找到」)。
#
# 判据(与游戏内一致):
#   · 喂给 scan() 的特征必须**恰好命中 1 处**;
#   · 喂给 scan_all() / 手写循环的特征**允许多处**(它们本来就是收一批同类指令的),
#     但至少要命中 1 处,并把处数报出来供人工核对(源码注释里记了这份构建的已知处数);
#   · 源码里写了期望地址的,命中地址必须等于它。
#
# 用法: dryscan.py <src/framelab.cpp> <RA3_1.12.game>
#   退出码 0 = 全部通过;1 = 有问题。

import re
import struct
import sys


# ── PE:取 ImageBase 与各段(与 pescan.py 同一套;不共用文件是为了让 re 工具各自独立可跑)──
def load_image(path):
    with open(path, "rb") as f:
        d = f.read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("FAIL: %s 不是 PE 文件" % path)
    coff = pe + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    optsz = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    ib = struct.unpack_from("<I", d, opt + 28)[0]
    secs = []
    so = opt + optsz
    for i in range(nsec):
        o = so + i * 40
        nm = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vs, va, rs, rp = struct.unpack_from("<IIII", d, o + 8)
        secs.append((nm, va, vs, rp, rs))
    return d, ib, secs


def compile_pattern(tokens):
    """tokens = ['0x8B','W','0xCE',...] -> (pat, mask)。mask 1=必须匹配,0=通配。"""
    pat, mask = bytearray(), bytearray()
    for t in tokens:
        t = t.strip()
        if t == "W":
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(t, 16) & 0xFF)
            mask.append(1)
    return bytes(pat), bytes(mask)


def find_all(text, pat, mask):
    hits = []
    n = len(pat)
    # 先用一段全确定的字节做锚点快速定位,再逐字节校验,避免 O(len(text)*len(pat))
    k = 0
    while k < n and mask[k]:
        k += 1
    anchor = pat[:k] if k else pat[:1]
    s = 0
    while True:
        i = text.find(anchor, s)
        if i < 0:
            break
        if all(not mask[j] or text[i + j] == pat[j] for j in range(n)):
            hits.append(i)
        s = i + 1
    return hits


# ── 解析 framelab.cpp ────────────────────────────────────────────────────────
def strip_comments(src):
    """去掉 // 行注释。这些数组里的注释都在行尾或独立成行,没有跨行 /* */。"""
    out = []
    for line in src.splitlines():
        i = line.find("//")
        out.append(line[:i] if i >= 0 else line)
    return "\n".join(out)


def parse_sig_arrays(src):
    """找 `static const short kSigXxx[] = { ... };` -> {名字: [token,...]}"""
    sigs = {}
    for m in re.finditer(r"static\s+const\s+short\s+(kSig\w+)\s*\[\s*\]\s*=\s*\{(.*?)\};",
                         src, re.S):
        name, body = m.group(1), m.group(2)
        toks = [t.strip() for t in body.split(",")]
        toks = [t for t in toks if t]
        bad = [t for t in toks if not (t == "W" or re.fullmatch(r"0x[0-9A-Fa-f]+", t))]
        if bad:
            raise SystemExit("FAIL: %s 里有看不懂的 token: %s" % (name, bad))
        sigs[name] = toks
    return sigs


def parse_patterns(src):
    """找 `Pattern pXxx = {"名字", kSigXxx, <长度>, 0xVA};` 形式的用例。

    ⚠ 2026-09-17:长度表达式在源码里有**两种**写法,第一版只认第一种,
    于是漏扫了动画标尺那一条(kSigAnimFractionCall),干扫报「15 条」而实际有 16 条
    —— 这种静默漏扫正是这个工具要防的东西,所以自己先修掉:
      ① (int)(sizeof kSigXxx / sizeof(short))          ← 绝大多数
      ② (int)(sizeof kSigXxx / sizeof kSigXxx[0])      ← 数组元素为 short 之外的类型时更稳
    """
    out = []
    for m in re.finditer(
            r"Pattern\s+(\w+)\s*=\s*\{\s*\"([^\"]*)\"\s*,\s*(kSig\w+)\s*,\s*"
            r"\(int\)\(sizeof\s+\w+\s*/\s*sizeof\s*\(?\s*(?:short|\w+\s*\[\s*0\s*\])\s*\)?\s*\)\s*,"
            r"\s*(0x[0-9A-Fa-f]+|0)\s*\}", src):
        out.append({"var": m.group(1), "name": m.group(2), "sig": m.group(3),
                    "expected": int(m.group(4), 16)})
    return out


def parse_single_hit_vars(src):
    """哪些 Pattern 变量是喂给 scan() 的(要求唯一命中)?

    不能一律要求「恰好 1 处」:scan() 定位单个补丁点,而 scan_all() / 手写循环本来就是
    用来收**多条**同类指令的(「算 r」这份构建已知 5 处、帧率派生量 3 处)。
    第一版脚本没区分,把 5 处和 14 处误报成 FAIL。
    """
    single = set()
    for m in re.finditer(r"(?<!_)\bscan\s*\(\s*(\w+)\s*\)", src):
        single.add(m.group(1))
    return single


def main():
    if len(sys.argv) < 3:
        raise SystemExit("用法: dryscan.py <src/framelab.cpp> <RA3_1.12.game>")
    srcpath, imgpath = sys.argv[1], sys.argv[2]

    with open(srcpath, "rb") as f:
        raw = f.read()
    try:
        src = strip_comments(raw.decode("utf-8-sig"))
    except UnicodeDecodeError:
        src = strip_comments(raw.decode("gbk", "replace"))

    sigs = parse_sig_arrays(src)
    pats = parse_patterns(src)
    single_vars = parse_single_hit_vars(src)
    print("解析到 %d 条特征数组,%d 个 Pattern 用例" % (len(sigs), len(pats)))
    if not pats:
        raise SystemExit("FAIL: 一个 Pattern 都没解析到 —— 源码格式变了,检查本脚本的正则")

    d, ib, secs = load_image(imgpath)
    text_va = text_size = text_off = None
    for nm, va, vs, rp, rs in secs:
        if nm == ".text":
            text_va, text_size, text_off = va, vs, rp
    if text_va is None:
        raise SystemExit("FAIL: 镜像里没有 .text 段")
    text = d[text_off:text_off + text_size]
    print(".text: VA 0x%08X + 0x%X,ImageBase 0x%08X" % (text_va, text_size, ib))
    print()

    bad = 0
    seen = set()
    for p in pats:
        key = (p["var"], p["sig"], p["name"])
        if key in seen:          # 同一个 Pattern 变量被复用了两次,报一次就够
            continue
        seen.add(key)

        toks = sigs.get(p["sig"])
        if toks is None:
            print("FAIL %-22s 找不到数组 %s" % (p["name"], p["sig"]))
            bad += 1
            continue
        want_single = p["var"] in single_vars
        pat, mask = compile_pattern(toks)
        hits = find_all(text, pat, mask)

        if not hits:
            print("FAIL %-22s 0 命中  [%s]  特征长度 %d 字节"
                  % (p["name"], p["sig"], len(toks)))
            bad += 1
            continue
        if want_single and len(hits) != 1:
            print("FAIL %-22s 命中 %d 处,但它是喂给 scan() 的,必须恰好 1 处  [%s]"
                  % (p["name"], len(hits), p["sig"]))
            bad += 1
            continue

        va = ib + text_va + hits[0]
        if p["expected"] and va != p["expected"]:
            print("FAIL %-22s 命中 0x%08X,源码写的期望是 0x%08X  [%s]"
                  % (p["name"], va, p["expected"], p["sig"]))
            bad += 1
            continue

        if want_single:
            note = "== 期望值" if p["expected"] else "(源码未写期望地址)"
            print("OK   %-22s 0x%08X 唯一命中 %s  [%s]" % (p["name"], va, note, p["sig"]))
        else:
            print("OK   %-22s 0x%08X 命中 %d 处(允许多处,首处在 0x%08X)  [%s]"
                  % (p["name"], va, len(hits), va, p["sig"]))

    print()
    if bad:
        print("---- %d 条不通过" % bad)
        return 1
    print("---- 全部通过(%d 条)。可以起游戏了。" % len(seen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
