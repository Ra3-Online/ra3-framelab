#!/usr/bin/env python3
# band_vshift.py -- measure VERTICAL shift per horizontal band, from a screen recording.
#
# 2026-09-18 (batch 11, follow-up). Why this exists:
#   The user reports that vehicles "jitter violently up and down" while moving, at 30 fps,
#   and sent a 6.6 s / 25 fps phone recording. video_jitter.py already tried to isolate one
#   vehicle by brightness and found "top edge barely moves, bottom edge moves a little" --
#   i.e. contour change, not a whole-body bounce. That answer was not actionable.
#
#   The problem with isolating a vehicle by brightness is that you must first know WHERE the
#   vehicle is, and on a hand-held phone recording of a bright RTS scene that is guesswork.
#   So this tool does not try. It measures the vertical motion of the WHOLE image, band by
#   band, and then compares the bands.
#
#   That comparison is the whole point:
#     * ALL bands move together with the same amplitude  -> the CAMERA moved (hand shake,
#       or the in-game camera). Not a vehicle bug.
#     * Only SOME bands move, others are flat            -> real CONTENT motion in that band.
#     * The residual after removing a smooth trend is the jitter, in PIXELS.
#
#   A phone recording of a screen cannot resolve the screen's frame timing (25 fps sampling a
#   30 Hz panel aliases), but it CAN measure how many pixels something moves between captured
#   frames. Amplitude survives aliasing; frequency does not. So we report amplitude and say so.
#
# How it works (one ffmpeg pass, no numpy):
#   ffmpeg -vf scale=1:H:flags=area  collapses each source row to its mean brightness, giving
#   an H-byte "row profile" per frame. A vertical shift of the content is a vertical shift of
#   that profile, so we cross-correlate consecutive profiles. Splitting the H rows into bands
#   afterwards costs nothing and gives us the per-band answer.
#
# Usage:
#   python band_vshift.py <video> [--height 1600] [--bands 8] [--search 25] [--out report.txt]
#
# FFMPEG env var overrides the ffmpeg binary.

import os
import subprocess
import sys

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def decode_profiles(video, height):
    """Yield one row-profile per frame: `height` bytes, byte i = mean brightness of row i."""
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-i", video,
           "-vf", "scale=1:%d:flags=area" % height,
           "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    while True:
        buf = p.stdout.read(height)
        if len(buf) < height:
            break
        yield buf
    p.stdout.close()
    p.wait()


def best_shift(a, b, search):
    """Integer vertical shift of b that best matches a, searching +-search samples.

    Returns (shift, score) where a positive shift means b's content sits LOWER than a's,
    i.e. the image moved DOWN by `shift` rows. Normalised so score is comparable across bands.
    """
    best_s, best_v = 0, None
    n = len(a)
    lo = search
    hi = n - search
    if hi <= lo:
        return 0, 0.0
    for s in range(-search, search + 1):
        # compare a[lo:hi] with b[lo+s:hi+s]
        acc = 0
        for i in range(lo, hi):
            d = a[i] - b[i + s]
            acc += d if d >= 0 else -d
        if best_v is None or acc < best_v:
            best_v, best_s = acc, s
    mean = 0.0
    for i in range(lo, hi):
        mean += a[i]
    mean /= (hi - lo)
    return best_s, (best_v / (hi - lo)) if best_v is not None else 0.0


def highpass(vals, half):
    """Subtract a centred moving average -> keeps only what is faster than ~1/(2*half+1)."""
    n = len(vals)
    out = []
    for i in range(n):
        lo = max(0, i - half)
        hi = min(n, i + half + 1)
        m = sum(vals[lo:hi]) / (hi - lo)
        out.append(vals[i] - m)
    return out


def stats(vals):
    if not vals:
        return (0.0, 0.0, 0.0)
    n = len(vals)
    mean = sum(vals) / n
    var = sum((v - mean) ** 2 for v in vals) / n
    return (mean, var ** 0.5, max(abs(v) for v in vals))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    video = sys.argv[1]

    def arg(name, default):
        return int(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default

    height = arg("--height", 1600)
    bands = arg("--bands", 8)
    search = arg("--search", 25)

    profs = list(decode_profiles(video, height))
    if len(profs) < 4:
        print("decoded only %d frames -- video unreadable?" % len(profs))
        return 1

    lines = []
    lines.append("=" * 78)
    lines.append("分带垂直位移分析 / band_vshift.py")
    lines.append("来源: %s" % video)
    lines.append("帧数 %d   每帧 %d 行   分 %d 带   搜索范围 +-%d 像素(原始分辨率)"
                 % (len(profs), height, bands, search))
    lines.append("=" * 78)
    lines.append("")
    lines.append("判读:同一带内的垂直位移如果只是缓慢漂移,那是镜头/手机在动,不是抖动。")
    lines.append("      高通残差才是「逐帧抖动」。★关键是比各带之间是否同步:")
    lines.append("        各带同步、幅度相近  -> 整幅画面在动(镜头/手机),不是载具问题")
    lines.append("        只有某几条带在动    -> 那一片内容真的在抖")
    lines.append("")

    bh = height // bands
    all_band_series = []
    for b in range(bands):
        y0 = b * bh
        y1 = y0 + bh
        series = [0]
        scores = []
        for i in range(1, len(profs)):
            s, sc = best_shift(profs[i - 1][y0:y1], profs[i][y0:y1], search)
            series.append(s)
            scores.append(sc)
        all_band_series.append(series)
        hp = highpass([float(v) for v in series], 4)
        m, sd, pk = stats(hp)
        drift = sum(series) / len(series)
        lines.append("带 %d  y=%4d..%4d  位移均值 %+6.2f px   高通残差 标准差 %5.2f px  峰值 %5.2f px"
                     % (b, y0, y1, drift, sd, pk))
        lines.append("        位移序列(前 60 帧): %s" % series[:60])

    # Cross-band agreement: correlation of the high-passed series between bands.
    lines.append("")
    lines.append("── 带间同步性(高通后两两相关系数)──")
    hp_series = [highpass([float(v) for v in s], 4) for s in all_band_series]
    hdr = "      " + "".join("%7d" % b for b in range(bands))
    lines.append(hdr)
    for i in range(bands):
        row = "带%3d " % i
        for j in range(bands):
            a, c = hp_series[i], hp_series[j]
            n = len(a)
            ma, mc = sum(a) / n, sum(c) / n
            num = sum((a[k] - ma) * (c[k] - mc) for k in range(n))
            da = sum((a[k] - ma) ** 2 for k in range(n)) ** 0.5
            dc = sum((c[k] - mc) ** 2 for k in range(n)) ** 0.5
            r = num / (da * dc) if da > 1e-9 and dc > 1e-9 else 0.0
            row += "%7.2f" % r
        lines.append(row)

    txt = "\n".join(lines) + "\n"
    print(txt)
    if "--out" in sys.argv:
        with open(sys.argv[sys.argv.index("--out") + 1], "w", encoding="utf-8") as f:
            f.write(txt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
