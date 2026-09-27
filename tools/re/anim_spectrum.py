#!/usr/bin/env python3
# tools/re/anim_spectrum.py -- 对某通道的「每帧推进量」做频谱/自相关分析。
#
# 为什么需要它(2026-09-20):
#   anim_trace.py 只能看出「有没有周期性」,看不出「周期是多少」。
#   而「抖动」的关键参数恰恰是**频率**:人眼对 20 Hz 上下的抖动最敏感,
#   对 60 Hz 的抖动反而容易忽略(会被当成平滑)。所以要先量出周期。
#
# 做法:
#   ① 取该通道的「每渲染帧推进量」序列(与 anim_trace 同口径:Δ ÷ 跨帧数);
#   ② 去均值后做 DFT,找主频;
#   ③ 同时给自相关,交叉验证。
#
# 用法:
#   anim_spectrum.py <snapshot.log> <通道号>
import io
import math
import re
import sys

ROW_RE = re.compile(
    r"^\s*(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+"
    r"(0x[0-9A-Fa-f]+)\s+(-?\d+)\s+(-?[\d.]+)\s+(-?\d+)"
    r"(?:\s+(-?\d+)((?:\s+-?[\d.]+){12}))?"
    r"(?:\s+(-?[\d.]+)((?:\s+-?[\d.]+){2}))?"
    r"\s*$")


def parse(path, ch):
    """返回 [(renderFrame, 墙钟us, 每帧推进量)]。"""
    rows = []
    with io.open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ROW_RE.match(line)
            if not m or not m.group(13):
                continue
            vals = [float(v) for v in m.group(13).split()]
            if ch >= len(vals) or vals[ch] < -0.5:
                continue
            rows.append((int(m.group(4)), int(m.group(2)), vals[ch]))
    out = []
    for i in range(1, len(rows)):
        r0, _, v0 = rows[i - 1]
        r1, t1, v1 = rows[i]
        dr = r1 - r0
        if dr <= 0:
            continue
        out.append((r1, t1, (v1 - v0) / dr))
    return out


def dft_peak(sig, dt_ms):
    """
    朴素 DFT(信号只有几百点,不必 FFT)。返回 (主频Hz, 该频幅度/总幅度)。
    只扫 0.5..30 Hz —— 这是「人眼能看到的抖动」的频段。
    """
    n = len(sig)
    mean = sum(sig) / n
    x = [s - mean for s in sig]
    total = sum(v * v for v in x)
    if total <= 1e-12:
        return 0.0, 0.0
    best_f, best_a = 0.0, 0.0
    f = 0.5
    while f <= 30.0:
        w = 2.0 * math.pi * f * (dt_ms / 1000.0)
        re = im = 0.0
        for i, v in enumerate(x):
            re += v * math.cos(w * i)
            im += v * math.sin(w * i)
        a = (re * re + im * im) / (n * n)
        if a > best_a:
            best_a, best_f = a, f
        f += 0.25
    return best_f, (best_a / (total / n))


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("用法: anim_spectrum.py <snapshot.log> <通道号>\n")
        return 2
    path, ch = sys.argv[1], int(sys.argv[2])
    seq = parse(path, ch)
    if len(seq) < 32:
        print("通道 %d 只有 %d 个有效样本,太少。" % (ch, len(seq)))
        return 1

    # 采样间隔(用中位,免得被偶发长帧带偏)。
    dts = sorted(seq[i][1] - seq[i - 1][1] for i in range(1, len(seq)))
    dt_ms = dts[len(dts) // 2] / 1000.0
    sig = [v for _, _, v in seq]

    print("通道 %d: %d 个样本,采样间隔中位 %.3f ms (≈ %.1f Hz)"
          % (ch, len(sig), dt_ms, 1000.0 / dt_ms if dt_ms > 0 else 0))
    mean = sum(sig) / len(sig)
    var = sum((v - mean) ** 2 for v in sig) / len(sig)
    print("推进量: 均值 %.5f  标准差 %.5f  (相对波动 %.2f%%)"
          % (mean, var ** 0.5, (var ** 0.5 / mean * 100.0) if mean > 1e-9 else 0))
    print("=" * 68)

    f, ratio = dft_peak(sig, dt_ms)
    print("★ 主频 %.2f Hz   该频能量占比 %.1f%%" % (f, ratio * 100.0))
    if ratio > 0.5 and 1.0 <= f <= 30.0:
        print("  ⇒ **明显的周期性成分** —— 这个频率的抖动是肉眼最容易看到的区间。")
        print("     周期 ≈ %.1f ms(每 %.1f 帧一次)" % (1000.0 / f, (1000.0 / f) / dt_ms))
    elif ratio > 0.25:
        print("  ⇒ 有周期性成分,但不占主导(其余是噪声/一次性跳变)。")
    else:
        print("  ⇒ 能量分散,没有主导频率 —— 更像随机噪声或一次性跳变,不是规律性抖动。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
