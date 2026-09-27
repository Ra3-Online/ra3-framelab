#!/usr/bin/env python3
# tools/re/anim_pair.py -- 把两臂的动画通道按「分数范围重叠」配对,再比推进量。
#
# 为什么需要它(2026-09-20):
#   黑匣子的动画轴一次记 12 个槽,但**槽的分配是「先到先得 + 淘汰最不活跃」**,
#   两次运行的时序不同 ⇒ 采到的对象不是同一批 ⇒ **跨臂比「绝对推进量」是没有意义的**。
#   本会话就栽在这上面:只看前 4 个通道时,30 帧有 1 个「连续推进」、原版全是「阶梯」,
#   看起来是补丁的铁证;扩到 12 个通道后,原版立刻出现了 2 个「连续推进」的通道,
#   而且其中一个的中位推进量(1.05600)与 30 帧那个(1.05590)**几乎相同**
#   ⇒ 两条结论全部作废。
#
#   ⇒ 正确的做法:先**按分数范围**把两臂的通道配成对(同一个对象在两次运行里
#     分数范围应当高度重叠),**在配对成功的那一对上**再比推进量。
#
# 用法:
#   anim_pair.py <A臂的 ring_analyze 报告> <B臂的 ring_analyze 报告> [--tol 0.05]
#
# 输出:每一对配上的通道,以及它们的「推进量」差异。
#   如果没有任何一对配得上 ⇒ 明确说「这一轮配不上」,而不是硬比。
import io
import re
import sys

# 从 ring_analyze 的报告里抠出每个通道的 (有效帧数, 分数下界, 分数上界, 中位|Δ|, p90|Δ|)
CH_RE = re.compile(
    r"动画通道\s+(\d+):\s+有效\s+(\d+)\s+帧\s+分数范围\s+(-?[\d.]+)\.\.(-?[\d.]+)")
D_RE = re.compile(r"★帧间 \|Δ\|:\s*中位\s+(-?[\d.]+)\s+p90\s+(-?[\d.]+)")


def parse(path):
    """返回 {通道号: dict(frames, lo, hi, med, p90)}。"""
    out = {}
    cur = None
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = CH_RE.search(line)
            if m:
                cur = int(m.group(1))
                out[cur] = {
                    "frames": int(m.group(2)),
                    "lo": float(m.group(3)),
                    "hi": float(m.group(4)),
                    "med": None,
                    "p90": None,
                }
                continue
            m = D_RE.search(line)
            if m and cur is not None:
                out[cur]["med"] = float(m.group(1))
                out[cur]["p90"] = float(m.group(2))
    return out


def overlap_frac(a, b):
    """
    「同一个对象」的判据:两臂的分数范围**两端都要接近**。
    ★ 为什么不用「区间重叠比例」:实测很多通道的范围是 `0..X`(下界都是 0),
      用重叠比例会让 `0..60.390` 和 `0..11.815` 判成「100% 重叠」—— 可它们上界差 5 倍,
      显然是不同对象。**下界相同不等于同一个对象。**
    ⇒ 改用「两端相对偏差」:两个端点各自除以两臂的跨度最大值,取较大者作为距离。
      距离 <= tol ⇒ 认为是同一个对象。这样 `0..60.39` vs `0..11.815` 的距离是
      |60.39-11.815| / 60.39 = 0.80 ⇒ 直接排除。
    """
    span = max(a["hi"] - a["lo"], b["hi"] - b["lo"], 1e-6)
    d_lo = abs(a["lo"] - b["lo"]) / span
    d_hi = abs(a["hi"] - b["hi"]) / span
    return max(d_lo, d_hi)          # 0 = 完全相同;越大越不像


def cont(rec):
    """连续度 = 中位 ÷ p90(与 ring_analyze 第 7 节同一条式子)。"""
    if rec["med"] is None or rec["p90"] is None or rec["p90"] <= 1e-9:
        return 0.0
    return rec["med"] / rec["p90"]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tol = 0.05
    for a in sys.argv[1:]:
        if a.startswith("--tol"):
            tol = float(a.split("=", 1)[1]) if "=" in a else 0.05
    if len(args) < 2:
        sys.stderr.write("用法: anim_pair.py <A臂报告> <B臂报告> [--tol=0.05]\n")
        return 2

    A = parse(args[0])
    B = parse(args[1])
    if not A or not B:
        sys.stderr.write("两臂报告里都没解析到动画通道(是不是旧日志?)\n")
        return 2

    print("A 臂: %s  (%d 个通道)" % (args[0], len(A)))
    print("B 臂: %s  (%d 个通道)" % (args[1], len(B)))
    print("配对判据: 分数范围重叠比例 >= %.2f" % (1.0 - tol))
    print("=" * 78)

    pairs = []
    for ca, ra in sorted(A.items()):
        best = None
        for cb, rb in sorted(B.items()):
            d = overlap_frac(ra, rb)          # 0 = 完全相同;越大越不像
            if best is None or d < best[1]:
                best = (cb, d, rb)
        if best and best[1] <= tol:
            pairs.append((ca, ra, best[0], best[2], best[1]))

    if not pairs:
        print("★ 没有任何一对通道配得上(两端偏差都超过 %.2f)。" % tol)
        print("  ⇒ 这一轮**不能**做跨臂比较 —— 不是「没差异」,是「采到的不是同一批对象」。")
        print("  建议:重跑一次(槽分配有时序随机性),或者把记录时间拉长让更多对象进槽。")
        return 1

    print("%-7s %-30s %-30s %7s %9s %9s %9s" %
          ("配对", "A 臂(范围 / 中位|Δ|)", "B 臂(范围 / 中位|Δ|)", "端点差", "A 连续度", "B 连续度", "推进量比"))
    for ca, ra, cb, rb, d in pairs:
        ratio = (ra["med"] / rb["med"]) if (ra["med"] and rb["med"] and rb["med"] > 1e-9) else float("nan")
        print("%-7s %-30s %-30s %7.3f %9.3f %9.3f %9.4f" % (
            "A%d-B%d" % (ca, cb),
            "%.3f..%.3f / %.5f" % (ra["lo"], ra["hi"], ra["med"] or 0.0),
            "%.3f..%.3f / %.5f" % (rb["lo"], rb["hi"], rb["med"] or 0.0),
            d, cont(ra), cont(rb), ratio))

    print("=" * 78)
    # 只有「两臂都是连续推进」的配对,推进量比才有意义(阶梯推进的中位恒为 0,比值无意义)。
    usable = [p for p in pairs if cont(p[1]) > 0.8 and cont(p[3]) > 0.8]
    if usable:
        print("★ 可用于比「推进量」的配对(两臂都是连续推进):%d 对" % len(usable))
        for ca, ra, cb, rb, ov in usable:
            r = ra["med"] / rb["med"] if rb["med"] > 1e-9 else float("nan")
            print("  A%d vs B%d: 中位 %.5f vs %.5f  ⇒ 比值 %.4f (偏差 %+.2f%%)" % (
                ca, cb, ra["med"], rb["med"], r, (r - 1.0) * 100.0))
    else:
        print("★ 配对上的通道里没有「两臂都连续推进」的 ⇒ 推进量不可比(阶梯的中位恒为 0)。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
