# ring_analyze.py -- turn a black-box snapshot/crash report into a jitter verdict.
#
# 2026-09-18 / batch 11 (Claude).  ASCII-only (PowerShell/console friendliness is not an issue
# here, but the project rule is: keep tooling files plain).
#
# WHY THIS EXISTS
#   The user reports "vehicles bounce up and down badly at 30 fps, and vanilla already had a
#   slight bounce". A phone video at 25 fps cannot measure a 30/60 Hz bounce (aliasing), so we
#   need an in-engine instrument. The black box already records, once per RENDER frame:
#       tick (wall clock ms) | logicFrame | renderFrame | clock(CE1388) | msPerFrame | r
#       groups | phase | frac | engineFps
#
#   The quantity that decides whether a moving object LOOKS jittery is not frac alone, it is
#   "which instant of world time is being presented on this render frame":
#
#       prog(n) = logicFrame(n) + frac(n)          [unit: logic frames]
#
#   A unit moving at constant velocity sits at position proportional to prog. The display shows
#   one sample per render frame, at wall-clock time tick(n). If prog advances LINEARLY with tick,
#   the motion is perfectly smooth. Any wiggle of prog around the best-fit line is exactly the
#   up/down bounce the eye sees. So:
#
#       residual(n) = prog(n) - (slope * tick(n) + intercept)
#
#   and the std / peak-to-peak of residual, converted to milliseconds (x 1000/logicFps) and to
#   "fraction of one frame", IS the jitter metric. It is comparable across arms because it is
#   normalised against wall-clock time.
#
#   CAVEAT, stated so nobody over-reads the number: a constant-velocity model is an idealisation.
#   A unit that accelerates, turns, or climbs terrain has a genuinely non-linear prog, and that
#   shows up as residual too. So this tool is only meaningful when compared BETWEEN ARMS on the
#   SAME replay: vanilla-30 vs patched-30 vs patched-60. The absolute number means little; the
#   ratio does. That is the same discipline as determinism.ps1 ("surviving is not proof").
#
# Usage:
#   python ring_analyze.py <snapshot-or-crash.log> [--logic-fps 15] [--out report.txt]
#
# Exit code 0 on success, 2 on parse failure.

import argparse
import math
import re
import sys

# 序号 墙钟ms 逻辑帧 渲染帧 时钟ms 每帧ms r 分组 阶段 插值 引擎FPS
ROW_RE = re.compile(
    r"^\s*(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+"
    r"(0x[0-9A-Fa-f]+)\s+(-?\d+)\s+(-?[\d.]+)\s+(-?\d+)"
    # ★ 2026-09-18(第十一批·续):动画轴两列 —— 本渲染帧的「切动画」钩子调用次数,
    #   以及前 4 个动画对象的当前动画分数。载具移动时的上下起伏(悬挂 bob)就是
    #   动画混合的输出,所以这几列**直接就是「载具上下」的读数**。
    #   ⚠ 写成**可选组**是刻意的:本批之前的日志没有这两列,必须继续能解析。
    r"(?:\s+(-?\d+)((?:\s+-?[\d.]+){12}))?"
    # ★ 2026-09-18(第十一批·续)第二批:世界坐标跟踪三列(x / y / z)。同样写成**可选组**,
    #   旧日志继续能解析。这三列由 `flctl track <addr>` 配置,是「移动时上下抖动」的
    #   唯一直接读数 —— 时间轴量具看不见位置,动画分数也看不见位置。
    r"(?:\s+(-?[\d.]+)((?:\s+-?[\d.]+){2}))?"
    r"\s*$"
)

STATE_RE = re.compile(
    r"补丁状态\s+已安装=(\d+)\s+目标帧率=(-?\d+)\s+r=(-?\d+)\s+分组=(0x[0-9A-Fa-f]+)"
)


def parse(path):
    """Return (state_dict, rows, unit). rows is a list of dicts, one per render frame.

    unit is "us" or "ms" and describes the timestamp column of the source file. Older reports
    (before 2026-09-18 batch 11) used GetTickCount, i.e. milliseconds with a 15.625 ms quantum --
    which is coarse enough to fabricate a +/-7 ms sawtooth in the frame-interval histogram. Newer
    reports use QueryPerformanceCounter in microseconds. We normalise everything to milliseconds
    so the two can be compared, but the caller should still prefer "us" reports.
    """
    with open(path, "r", encoding="utf-8-sig", errors="replace") as fh:
        text = fh.read()

    unit = "us" if "墙钟us" in text else "ms"

    state = {}
    m = STATE_RE.search(text)
    if m:
        state = {
            "installed": int(m.group(1)),
            "target_fps": int(m.group(2)),
            "r": int(m.group(3)),
            "groups": m.group(4),
            "unit": unit,
        }

    scale = 0.001 if unit == "us" else 1.0
    rows = []
    for line in text.splitlines():
        mm = ROW_RE.match(line)
        if not mm:
            continue
        rows.append({
            "idx": int(mm.group(1)),
            "tick": int(mm.group(2)) * scale,      # milliseconds, float
            "logic": int(mm.group(3)),
            "render": int(mm.group(4)),
            "clock": int(mm.group(5)),
            "mspf": int(mm.group(6)),
            "ratio": int(mm.group(7)),
            "groups": mm.group(8),
            "phase": int(mm.group(9)),
            "frac": float(mm.group(10)),
            "efps": int(mm.group(11)),
            # 动画轴。旧日志没有这两列 -> None,后面的第 7 节会跳过。
            "animcalls": int(mm.group(12)) if mm.group(12) else None,
            "anim": [float(v) for v in mm.group(13).split()] if mm.group(13) else None,
            # 世界坐标跟踪 (x,y,z)。旧日志为 None,第 8 节会跳过。
            "trk": ([float(mm.group(14))] + [float(v) for v in mm.group(15).split()])
                   if mm.group(15) else None,
        })
    return state, rows


def pct(sorted_vals, p):
    if not sorted_vals:
        return 0.0
    k = (len(sorted_vals) - 1) * p / 100.0
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    if lo == hi:
        return float(sorted_vals[lo])
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def histogram(values, bins=12):
    if not values:
        return []
    lo, hi = min(values), max(values)
    if hi == lo:
        return [(lo, hi, len(values))]
    width = (hi - lo) / bins
    counts = [0] * bins
    for v in values:
        i = min(bins - 1, int((v - lo) / width))
        counts[i] += 1
    return [(lo + i * width, lo + (i + 1) * width, counts[i]) for i in range(bins)]


def run_lengths(values):
    """[(value, count), ...] in order -- used to show repeating patterns compactly."""
    out = []
    for v in values:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return [(v, c) for v, c in out]


def best_fit_residual(xs, ys):
    """Least-squares line ys ~ a*xs + b, returning (a, b, residuals)."""
    n = len(xs)
    if n < 2:
        return 0.0, 0.0, [0.0] * n
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    a = num / den if den else 0.0
    b = my - a * mx
    return a, b, [y - (a * x + b) for x, y in zip(xs, ys)]


def highpass(vals, half=4):
    """Subtract a centred moving average, keeping only the frame-to-frame wiggle.

    WHY THIS IS NEEDED -- learned the hard way on 2026-09-18:
    A 512-frame record can be dominated by a SLOW drift. If the whole run happens to render 1%
    under target, prog falls behind wall clock steadily, and the least-squares residual comes out
    as a near-monotone ramp whose standard deviation reaches 15 ms. That number looks like
    terrible jitter but it is not jitter at all -- it is "this run was slightly slow". The eye
    never sees a 15 ms ramp across 17 seconds; it sees the frame-to-frame wiggle, which is
    exactly what survives this high-pass filter.

    So both numbers are reported and they answer different questions:
      - low-frequency (the raw residual): how far the presentation drifts from real time
      - high-frequency (this filter):     how much the motion wobbles frame to frame  <-- the eye
    """
    n = len(vals)
    out = []
    for i in range(n):
        a = max(0, i - half)
        b = min(n, i + half + 1)
        out.append(vals[i] - sum(vals[a:b]) / (b - a))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--logic-fps", type=float, default=15.0,
                    help="logic frames per second (RA3 retail = 15)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    state, rows = parse(args.log)
    out_lines = []

    def emit(s=""):
        out_lines.append(s)

    emit("=" * 78)
    emit("黑匣子环形缓冲分析 / ring_analyze.py")
    emit("来源: %s" % args.log)
    if state:
        emit("补丁状态: 已安装=%(installed)d 目标帧率=%(target_fps)d r=%(r)d 分组=%(groups)s"
             % state)
        emit("时间戳源: %s" % ("QueryPerformanceCounter(微秒) —— 可信"
                               if state.get("unit") == "us"
                               else "GetTickCount(毫秒,15.625ms 量化) —— ★帧间隔数字不可信"))
    emit("记录帧数: %d" % len(rows))
    if len(rows) < 8:
        emit("")
        emit("记录太少(需要至少 8 行才有统计意义)。")
        emit("原因通常是:① 黑匣子没开 ② 开了之后没等够时间 ③ 这一局还没进对局。")
        text = "\n".join(out_lines)
        print(text)
        if args.out:
            open(args.out, "w", encoding="utf-8").write(text)
        return 2

    emit("=" * 78)
    emit("")

    # ---- 1. 渲染帧间隔(墙钟) ------------------------------------------------------------
    #     这是「呈现本身抖不抖」的下限。若间隔本身在 16/50 ms 之间跳,后面所有分析都没意义。
    ticks = [r["tick"] for r in rows]
    dts = [ticks[i + 1] - ticks[i] for i in range(len(ticks) - 1)]
    dts = [d for d in dts if d >= 0]
    sdt = sorted(dts)
    emit("── 1. 渲染帧间隔(墙钟 ms) ──  n=%d" % len(dts))
    if dts:
        emit("   平均 %.3f   最小 %.3f   p50 %.3f   p90 %.3f   p99 %.3f   最大 %.3f"
             % (sum(dts) / len(dts), min(dts), pct(sdt, 50), pct(sdt, 90), pct(sdt, 99), max(dts)))
        emit("   等效帧率 %.2f 帧/秒" % (1000.0 / (sum(dts) / len(dts)) if sum(dts) else 0))
        # 间隔的标准差是「呈现是否平稳」的直接证据。注意:若时间戳是毫秒量化的旧报告,
        # 这里的数会被刻度噪声抬起来 —— 看上面那行「时间戳源」。
        var = sum((d - sum(dts) / len(dts)) ** 2 for d in dts) / len(dts)
        emit("   间隔标准差 %.3f ms  (%.2f%% of mean)"
             % (math.sqrt(var), 100.0 * math.sqrt(var) / (sum(dts) / len(dts))))
        for lo, hi, c in histogram(dts):
            bar = "#" * min(60, c)
            emit("   [%7.3f .. %7.3f) %5d  %s" % (lo, hi, c, bar))
    emit("")

    # ---- 2. 逻辑帧推进 ----------------------------------------------------------------
    #     每渲染帧逻辑帧前进多少。15 Hz 逻辑 + 30 fps 渲染 应为 0/1 交替(平均 0.5);
    #     15 Hz + 60 fps 应为 0/0/0/1(平均 0.25)。出现 >1 的跳变 = 掉帧或 sim 卡顿。
    dl = [rows[i + 1]["logic"] - rows[i]["logic"] for i in range(len(rows) - 1)]
    dl = [d for d in dl if -1000 < d < 1000]
    emit("── 2. 每渲染帧的逻辑帧增量 ──")
    if dl:
        counts = {}
        for d in dl:
            counts[d] = counts.get(d, 0) + 1
        for d in sorted(counts):
            emit("   +%-4d  %5d 次  (%.1f%%)" % (d, counts[d], 100.0 * counts[d] / len(dl)))
        emit("   平均 %.4f 逻辑帧/渲染帧  ⇒ 逻辑帧率 %.3f Hz(名义 %.1f)"
             % (sum(dl) / len(dl), (sum(dl) / len(dl)) * (1000.0 / (sum(dts) / len(dts)) if dts else 0),
                args.logic_fps))
    emit("")

    # ---- 3. 插值系数序列 --------------------------------------------------------------
    fracs = [r["frac"] for r in rows if r["frac"] >= 0.0]
    emit("── 3. 插值系数 frac ──")
    if fracs:
        fcount = {}
        for f in fracs:
            fcount[round(f, 4)] = fcount.get(round(f, 4), 0) + 1
        for f in sorted(fcount):
            emit("   %.4f  %5d 次" % (f, fcount[f]))
        emit("   相邻帧的 |Δfrac| 中位数 %.4f,均值 %.4f"
             % (pct(sorted(abs(fracs[i + 1] - fracs[i]) for i in range(len(fracs) - 1)), 50),
                (sum(abs(fracs[i + 1] - fracs[i]) for i in range(len(fracs) - 1)) / max(1, len(fracs) - 1))))
    emit("")

    # ---- 4. msPerFrame / 游戏时钟增量 -------------------------------------------------
    #     msPerFrame 是引擎每客户端帧推进游戏时钟的量(FL_G_CLOCK 的累加器输出)。
    #     原版在 30 帧下恒为 33;补丁在 r=2 下应为 33,33,34 循环(数学上无偏,逐帧有 ±1 ms 摆)。
    #     若这里出现更大的摆动,或者时钟增量与 msPerFrame 对不上,动画就会逐帧抖。
    ms = [r["mspf"] for r in rows]
    emit("── 4. 每帧毫秒 g_msPerFrame ──")
    if ms:
        mcount = {}
        for v in ms:
            mcount[v] = mcount.get(v, 0) + 1
        for v in sorted(mcount):
            emit("   %4d ms  %5d 次" % (v, mcount[v]))
        emit("   周期序列(前 40 帧): %s" % (ms[:40],))
    emit("")
    ck = [r["clock"] for r in rows]
    dck = [ck[i + 1] - ck[i] for i in range(len(ck) - 1)]
    emit("── 5. 游戏时钟 dword_CE1388 增量 ──")
    if dck:
        ccount = {}
        for v in dck:
            ccount[v] = ccount.get(v, 0) + 1
        for v in sorted(ccount):
            emit("   %+5d ms  %5d 次" % (v, ccount[v]))
        emit("   前 40 帧增量: %s" % (dck[:40],))
    emit("")

    # ---- 6. ★ 抖动判据:呈现时刻 prog = logicFrame + frac ------------------------------
    #     移动物体在屏幕上的位置正比于 prog。prog 相对墙钟的最佳拟合线的残差 = 眼睛看到的抖。
    #     只取逻辑帧在推进、且 frac 有效的连续段(开局/暂停段 prog 不动,会把拟合带歪)。
    seg = [r for r in rows if r["logic"] >= 0 and r["frac"] >= 0.0]
    # 裁掉两端各 5% 的过渡段
    if len(seg) > 40:
        k = max(1, len(seg) // 20)
        seg = seg[k:-k]
    emit("── 6. ★抖动判据:呈现时刻 prog = 逻辑帧 + frac ──")
    if len(seg) >= 8:
        xs = [float(r["tick"]) for r in seg]
        ys = [r["logic"] + r["frac"] for r in seg]
        span_logic = ys[-1] - ys[0]
        span_ms = xs[-1] - xs[0]
        if span_ms <= 0 or span_logic <= 0:
            emit("   这一段 prog 没有推进(可能在暂停/加载),跳过。")
        else:
            a, b, res = best_fit_residual(xs, ys)
            ms_per_logic = 1000.0 / args.logic_fps
            res_ms = [v * ms_per_logic for v in res]
            sres = sorted(abs(v) for v in res_ms)
            emit("   拟合斜率 %.6f 逻辑帧/毫秒(= %.3f 逻辑帧/秒,名义 %.1f)"
                 % (a, a * 1000.0, args.logic_fps))
            emit("   低频(整体速率偏差): 斜率与名义值差 %.3f%%  ⇒ 这一段整体比名义%s"
                 % (100.0 * (a * 1000.0 - args.logic_fps) / args.logic_fps,
                    "快" if a * 1000.0 > args.logic_fps else "慢"))
            emit("   残差(ms):  标准差 %.3f   p50 %.3f   p90 %.3f   p99 %.3f   峰值 %.3f"
                 % (math.sqrt(sum(v * v for v in res_ms) / len(res_ms)),
                    pct(sres, 50), pct(sres, 90), pct(sres, 99), max(sres)))
            emit("   残差(逻辑帧): 标准差 %.5f   峰值 %.5f"
                 % (math.sqrt(sum(v * v for v in res) / len(res)), max(abs(v) for v in res)))
            emit("   残差(一帧的几分之一): 标准差 %.4f 帧   峰值 %.4f 帧"
                 % (math.sqrt(sum(v * v for v in res) / len(res)) * 6.0,
                    max(abs(v) for v in res) * 6.0))
            emit("")
            # ★ 高通:滤掉「整局跑得快/慢」的慢漂移,只留逐帧抖动。眼睛看的是这一个。
            hp = highpass(res_ms)
            shp = sorted(abs(v) for v in hp)
            emit("   ★高频(逐帧抖动,已滤掉整体速率偏差):")
            emit("       标准差 %.3f ms   p50 %.3f   p90 %.3f   p99 %.3f   峰值 %.3f ms"
                 % (math.sqrt(sum(v * v for v in hp) / len(hp)),
                    pct(shp, 50), pct(shp, 90), pct(shp, 99), max(shp)))
            emit("       换算成一帧的几分之一: 标准差 %.4f 帧   峰值 %.4f 帧"
                 % (math.sqrt(sum(v * v for v in hp) / len(hp)) / ms_per_logic,
                    max(shp) / ms_per_logic))
            emit("")
            emit("   残差序列(前 60 帧,ms): %s" % ([round(v, 2) for v in res_ms[:60]],))
            emit("   高通序列(前 60 帧,ms): %s" % ([round(v, 2) for v in hp[:60]],))
            emit("")
            emit("   ★判读:同一份录像、不同配置之间比**高频标准差**。绝对数受单位加减速影响,")
            emit("     比例才是证据。补丁把抖动放大了几倍,这里就会差几倍。")
            emit("     残差(低频)那一行说的是「这一局整体跑得快还是慢」,不是抖动。")
    else:
        emit("   有效样本不足。")
    emit("")

    # ── 7. 动画轴:载具上下抖动的**直接**读数 ────────────────────────────────
    #   为什么单列一节:上面 6 节**全是时间轴**(呈现时刻、帧号、游戏时钟)。而用户报的
    #   「载具移动时上下抖动」是**动画轴**的现象 —— 时间轴量具在结构上就看不见它。
    #   更要紧的是,时间轴读数在这台多会话共用的机器上被外部负载主导(同一配置
    #   `30@0x28FFF` 在第 1 臂测到 3.844 ms、第 3 臂测到 0.411 ms,差 9 倍),
    #   而**动画分数是引擎算出来的确定性输出**,不随负载漂移 ⇒ 这一节才是可信证据。
    ac = [r["animcalls"] for r in seg if r["animcalls"] is not None]
    emit("── 7. ★动画轴:「载具上下抖动」的直接读数 ──")
    if not ac:
        emit("   本份日志没有动画轴列(本批之前的日志),跳过。")
        emit("")
    else:
        vac = [v for v in ac if v >= 0]
        if vac:
            svac = sorted(vac)
            emit("   切动画钩子调用次数/渲染帧: 平均 %.1f  中位 %d  p90 %d  最大 %d"
                 % (sum(vac) / len(vac), pct(svac, 50), pct(svac, 90), svac[-1]))
            emit("     ★这一列量化 FL_G_ANIMPROBE 的开销:该钩子每次调用都要跑 2 次")
            emit("       mem::read_ok(),而 read_ok 走 VirtualQuery(系统调用)。")
            emit("       每帧几百次以上 ⇒ 每帧几百次系统调用 ⇒ 帧时间被拉长并抖动。")
            emit("       对照:本项目在别处早已避开这个坑(bb::tick 读逻辑帧号就不用 read_ok)。")
        else:
            emit("   切动画钩子:整段都是 -1 ⇒ 动画标尺没装(FL_G_ANIMPROBE 关)。")
        emit("")

        # 动画分数通道。判据分两层:
        #   ① run_lengths 的中位长度 —— 30 帧下逻辑帧率是 15 Hz,而动画按逻辑帧推进,
        #      所以**正常的**表现是「每 2 个渲染帧同一个分数」(阶梯)。中位长度 ≈ 2 = 正常。
        #      中位长度 ≈ 1(每帧都在变)或忽长忽短 = 采样/推进出了问题。
        #   ② 高通残差 —— 滤掉阶梯的正常推进后还剩多少**无规律的**跳变。这才是「抖动」。
        # ★★ 2026-09-20:通道数改成**动态**。原来硬编码 4,而 Rec.anim 已扩到 FL_ANIM_OWNERS(12)
        #    —— 目的是让两臂有机会用「分数范围重叠」配到同一个动画对象。硬编码会让新增的
        #    8 个通道**静默不参与分析**(看起来像「没有更多通道」,其实是我们自己没读)。
        _nch = 0
        for _r in seg:
            if _r["anim"]:
                _nch = max(_nch, len(_r["anim"]))
        for i in range(_nch):
            vals = [r["anim"][i] for r in seg
                    if r["anim"] is not None and r["anim"][i] is not None and r["anim"][i] >= -0.5]
            if len(vals) < 8:
                emit("   动画通道 %d: 有效 %d 帧(太少,跳过)" % (i, len(vals)))
                continue
            # 分数只保留 5 位小数,量化到 1e-4 再判「相同」,免得浮点尾数把 run 打碎。
            # ★run_lengths 返回的是 [(value, count), ...],这里只要 count。
            q = [round(v, 4) for v in vals]
            rl = [c for _, c in run_lengths(q)]
            srl = sorted(rl)
            d = [abs(q[k] - q[k - 1]) for k in range(1, len(q))]
            sd = sorted(d)
            hp = highpass(vals, 4)
            shp = sorted(abs(v) for v in hp)
            emit("   动画通道 %d: 有效 %d 帧   分数范围 %.4f..%.4f"
                 % (i, len(vals), min(vals), max(vals)))
            emit("      ★连续同值长度: 中位 %d   p90 %d   最长 %d"
                 % (pct(srl, 50), pct(srl, 90), srl[-1]))
            emit("      ★帧间 |Δ|: 中位 %.5f   p90 %.5f   最大 %.5f"
                 % (pct(sd, 50), pct(sd, 90), sd[-1]))
            # ★★★ 推进形态(2026-09-19 动画方向会话加) —— 这一节现在最重要的一个数。
            #   连续度 = 帧间|Δ|中位 ÷ p90:
            #     ≈1 ⇒ 每渲染帧都在推进(**连续插值**)
            #     ≈0 ⇒ 隔帧才推进(逻辑帧 15 Hz / 渲染 30 Hz 的**阶梯**)
            #   实测:30 帧下有一个通道连续度 = 1.0559/1.0561 = 0.9998(连续);
            #   而**原版基线所有通道都是 0**(阶梯)。这是补丁改变动画推进方式的直接证据,
            #   而且它**不依赖两次运行采到同一个对象** —— 只看「形态」,不看绝对值。
            _med = pct(sd, 50)
            _p90 = pct(sd, 90)
            _cont = (_med / _p90) if _p90 > 1e-9 else 0.0
            _shape = ("连续推进(每渲染帧都动)" if _cont > 0.8 else
                      ("阶梯推进(隔帧才动)" if _cont < 0.2 else "混合"))
            emit("      ★推进形态: 连续度 = 中位 ÷ p90 = %.5f ÷ %.5f = %.3f  ⇒ %s"
                 % (_med, _p90, _cont, _shape))
            emit("      高通残差: 标准差 %.5f   峰值 %.5f"
                 % (math.sqrt(sum(v * v for v in hp) / len(hp)), max(shp)))
        emit("")
        emit("   ★判读:①连续同值长度中位 ≈ 2 是**正常**的 —— 逻辑帧率 15 Hz、渲染 30 Hz,")
        emit("     动画分数每 2 个渲染帧才推进一次,本来就是阶梯。")
        emit("     ②真正要找的抖动在高通残差里:阶梯是规律的,抖动是不规律的。")
        emit("     ③切动画调用次数是「帧时间被系统调用拉长」的直接证据。")
    emit("")

    # ── 8. 世界坐标跟踪:「移动时上下抖动」的正主 ─────────────────────────────
    #   为什么单列一节:第 6 节是时间轴,第 7 节是动画轴,**都看不见位置**。
    #   而用户答「移动时抖,静止时不抖」⇒ 问题就在**位置**这条路径上。
    #   这三列由 `flctl track <addr>` 配置(memscan 扫出来的「正在移动的三个相邻 float」)。
    trk = [r["trk"] for r in seg if r["trk"] is not None]
    emit("── 8. ★世界坐标跟踪 ──")
    if not trk:
        emit("   本份日志没有坐标列(没配 `flctl track <addr>`),跳过。")
        emit("")
    else:
        names = ("X", "Y", "Z")
        for k in range(3):
            vals = [t[k] for t in trk]
            if len(vals) < 8:
                emit("  分量 %s: 有效 %d 帧(太少,跳过)" % (names[k], len(vals)))
                continue
            d = [vals[i] - vals[i - 1] for i in range(1, len(vals))]
            sd = sorted(abs(v) for v in d)
            hp = highpass(vals, 4)
            shp = sorted(abs(v) for v in hp)
            total = vals[-1] - vals[0]
            emit("  分量 %s: 范围 %.4f .. %.4f   整段净位移 %+.4f"
                 % (names[k], min(vals), max(vals), total))
            emit("     帧间 |Δ|: 中位 %.5f   p90 %.5f   最大 %.5f"
                 % (pct(sd, 50), pct(sd, 90), sd[-1]))
            # ★★ 抖动倍数(2026-09-19 加):**匀速**移动时,每帧位移应当 ≈ |净位移| / 帧数。
            #    实测中位远大于它 ⇒ 位置在**来回振荡** —— 这就是眼睛看到的「抖动」。
            #    这个比值是**无量纲**的,所以可以直接拿两次运行(30 帧 vs 原版)的
            #    **同一个分量**做对比,不需要两次运行取到完全相同的绝对坐标。
            #    实测样例(用户专门录的 90 FPS Test):X 匀速应 0.29/帧、实测中位 2.06(7 倍);
            #    Z 匀速应 0.0002/帧、实测中位 0.036(**180 倍**)—— 高度分量的相对抖动最大,
            #    正对应「上下抖动」这个描述。
            nstep = len(vals) - 1
            if nstep > 0 and abs(total) > 1e-6:
                even = abs(total) / nstep
                ratio = (pct(sd, 50) / even) if even > 1e-9 else 0.0
                emit("     ★抖动倍数 = 帧间|Δ|中位 ÷ 匀速应有的每帧位移 = %.5f ÷ %.5f = **%.1f 倍**"
                     % (pct(sd, 50), even, ratio))
            else:
                emit("     ★抖动倍数: 净位移≈0(该分量整体没动),无法归一化 —— "
                     "改用残差绝对值比较")
            emit("     ★高通残差(滤掉整体移动后剩下的): 标准差 %.5f   峰值 %.5f"
                 % (math.sqrt(sum(v * v for v in hp) / len(hp)), max(shp)))
        emit("")
        emit("   ★判读:①「整段净位移」说明它到底有没有在移动(≈0 = 静止,这一节就白采了)。")
        emit("     ②**高通残差才是抖动**:载具匀速前进时位置应当平滑推进,滤掉趋势后应当≈0;")
        emit("       残差大 = 位置在振荡 = 眼睛看到的「上下跳」。")
        emit("     ③★**比较三个分量**:如果只有一个分量(通常是高度那一个)的残差远大于其它两个,")
        emit("       那就是「上下抖动」而不是「水平抖动」—— 这条对照是本节的全部价值。")
        emit("     ④同一份录像、30 帧 vs 原版 30 帧,比**同一个分量**的残差,比例才是证据。")
    emit("")

    emit("=" * 78)
    text = "\n".join(out_lines)
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print("\n[written] %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
