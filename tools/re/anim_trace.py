#!/usr/bin/env python3
# tools/re/anim_trace.py -- 把某个动画通道的**逐帧序列**打出来,并分析它的「跳变节律」。
#
# 为什么需要它(2026-09-20):
#   anim_pair.py 给出的是**统计量**(中位 |Δ|),它能告诉你「两臂的平均推进量差多少」,
#   但**不能**告诉你「这个差异在时间上是怎么分布的」。
#   而「抖动」恰恰是**时间上的分布问题**:
#     如果推进量是恒定的小数(比如 1.0559),那动画帧号会**匀速漂移**,
#     每隔若干帧必然要「补一跳」才能对齐 —— 那一跳就是肉眼看到的抖动。
#   ⇒ 本脚本输出「跳变间隔」的分布:
#       间隔**恒定** ⇒ 周期性跳变 ⇒ 就是抖动的直接机制
#       间隔**随机** ⇒ 只是噪声,不构成可见抖动
#
# 用法:
#   anim_trace.py <snapshot.log> <通道号> [--min-jump 0.5]
#
# 判据说明:
#   `--min-jump` 是「算作一次跳变」的最小帧间变化量。
#   0.5 是一个合理的默认:动画分数在 15Hz 逻辑帧下每 2 个渲染帧推进 1 次,
#   正常推进量约 1.0;明显大于正常值的单帧变化才算「跳」。
import io
import re
import sys

# 与 ring_analyze.py 同一条正则(动画段是「一组 float」,个数通配)。
ROW_RE = re.compile(
    r"^\s*(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+"
    r"(0x[0-9A-Fa-f]+)\s+(-?\d+)\s+(-?[\d.]+)\s+(-?\d+)"
    r"(?:\s+(-?\d+)((?:\s+-?[\d.]+){12}))?"
    r"(?:\s+(-?[\d.]+)((?:\s+-?[\d.]+){2}))?"
    r"\s*$")


def parse_rows(path):
    rows = []
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ROW_RE.match(line)
            if not m:
                continue
            anim = None
            if m.group(13):
                anim = [float(v) for v in m.group(13).split()]
            rows.append({
                "idx": int(m.group(1)),
                "tick": int(m.group(2)),          # us
                "logic": int(m.group(3)),
                "render": int(m.group(4)),
                "anim": anim,
            })
    return rows


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0
    k = int(round((len(sorted_vals) - 1) * p / 100.0))
    return sorted_vals[k]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    min_jump = 0.5
    for a in sys.argv[1:]:
        if a.startswith("--min-jump"):
            min_jump = float(a.split("=", 1)[1]) if "=" in a else 0.5
    if len(args) < 2:
        sys.stderr.write("用法: anim_trace.py <snapshot.log> <通道号> [--min-jump=0.5]\n")
        return 2
    path = args[0]
    ch = int(args[1])

    rows = parse_rows(path)
    if not rows:
        sys.stderr.write("没解析到任何数据行(是不是旧格式的日志?)\n")
        return 2

    # 只取该通道「有效」的帧(探针采到过的;-1.0 表示没采到)。
    seq = []
    for r in rows:
        if r["anim"] is None or ch >= len(r["anim"]):
            continue
        v = r["anim"][ch]
        if v < -0.5:                      # -1.0 = 该帧没采到这个槽
            continue
        seq.append((r["idx"], r["logic"], r["render"], v))

    if len(seq) < 20:
        print("通道 %d 只有 %d 个有效帧,太少(需要 >=20)。" % (ch, len(seq)))
        return 1

    print("通道 %d: 有效 %d 帧" % (ch, len(seq)))
    print("分数范围 %.4f .. %.4f" % (min(v for _, _, _, v in seq), max(v for _, _, _, v in seq)))
    print("=" * 74)

    # 逐帧差分。**注意**:只在「相邻有效帧」之间算,而且要求渲染帧号也相邻,
    # 否则中间缺帧会把「缺帧的累积量」算成一次大跳。
    diffs = []           # (idx, render, dv, dr)  dr = 渲染帧间隔
    for i in range(1, len(seq)):
        i0, l0, r0, v0 = seq[i - 1]
        i1, l1, r1, v1 = seq[i]
        dr = r1 - r0
        if dr <= 0:
            continue
        diffs.append((i1, r1, v1 - v0, dr))

    if not diffs:
        print("没有可用的相邻帧对。")
        return 1

    # 归一化到「每渲染帧推进量」——这样缺帧也不会污染统计。
    per_frame = sorted(d / dr for _, _, d, dr in diffs if dr > 0)
    print("每渲染帧推进量: 中位 %.5f  p90 %.5f  最大 %.5f" % (
        pct(per_frame, 50), pct(per_frame, 90), per_frame[-1]))

    # 跳变 = 单帧变化量明显大于 min_jump(按 dr 归一化后再乘 dr)。
    jumps = [(i, r, d, dr) for i, r, d, dr in diffs if abs(d) >= min_jump]
    print("跳变(单帧变化 >= %.2f): %d 次 / %d 个相邻帧对  ⇒ 每 %.1f 帧一次" % (
        min_jump, len(jumps), len(diffs),
        (len(diffs) / len(jumps)) if jumps else float("inf")))
    print("=" * 74)

    if not jumps:
        print("★ 没有跳变 ⇒ 这个通道在记录窗口内是**平稳推进**的,不构成可见抖动。")
        return 0

    # 跳变间隔的分布 —— 这是判断「周期性 vs 随机」的关键。
    gaps = []
    for i in range(1, len(jumps)):
        gaps.append(jumps[i][1] - jumps[i - 1][1])     # 渲染帧号之差
    print("跳变间隔(渲染帧): ", end="")
    print(", ".join(str(g) for g in gaps[:40]), end="")
    if len(gaps) > 40:
        print(" ... (共 %d 个)" % len(gaps))
    else:
        print()

    if gaps:
        sg = sorted(gaps)
        med = pct(sg, 50)
        mean = sum(gaps) / float(len(gaps))
        # 变异系数:标准差 / 均值。<0.25 视为「规律」,>0.5 视为「随机」。
        var = sum((g - mean) ** 2 for g in gaps) / len(gaps)
        cv = (var ** 0.5) / mean if mean > 1e-9 else float("inf")
        print("间隔统计: 中位 %d  均值 %.2f  最小 %d  最大 %d  变异系数 %.3f" % (
            med, mean, sg[0], sg[-1], cv))
        print("=" * 74)
        if cv < 0.25:
            print("★★★ **间隔高度规律** ⇒ 这是**周期性跳变** —— 正是肉眼可见的「抖动」的机制。")
            print("     解释:推进量是恒定小数(如 1.0559),动画帧号匀速漂移,")
            print("     每隔约 %d 帧就必须「补一跳」才能对齐 ⇒ 那一跳就是抖动。" % med)
        elif cv < 0.5:
            print("★ 间隔比较规律(变异系数 %.3f)⇒ 有周期性成分,但不是严格周期。" % cv)
        else:
            print("★ 间隔随机(变异系数 %.3f)⇒ 更像噪声,不构成规律性抖动。" % cv)

    # 再给一段原始序列,便于人眼核对。
    print("")
    print("前 30 个跳变的原始序列(渲染帧: 变化量):")
    for i, r, d, dr in jumps[:30]:
        print("  帧 %5d   Δ=%+9.5f   (跨 %d 帧)" % (r, d, dr))
    return 0


if __name__ == "__main__":
    sys.exit(main())
