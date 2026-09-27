#!/usr/bin/env python3
# video_jitter.py -- measure vertical jitter from a screen recording instead of eyeballing it.
#
# 2026-09-18 (batch 11). Why this exists:
#   The user reports that vehicles "jitter violently up and down" while moving, and sent a
#   6.6 s / 25 fps phone recording of a 60 Hz screen. Eyeballing a 25 fps recording of a
#   60 Hz screen is exactly how you end up with a confident wrong answer -- 25 fps aliases
#   anything that wobbles faster than ~12 Hz, and a hand-held phone adds its own motion.
#
#   So: decode one fixed crop as raw RGB, isolate the vehicle by brightness (RA3 vehicles are
#   cream/white against mid-tone grass), and turn the vehicle's vertical position into ONE
#   NUMBER PER FRAME. Then subtract a smooth trend (moving average). A vehicle moving in a
#   straight line leaves a near-zero residual; anything left at high frequency IS the jitter,
#   and its amplitude is reported in pixels.
#
# Usage:
#   python video_jitter.py <video> <WxH+X+Y> [--thresh 175] [--out report.txt]
#
# It shells out to ffmpeg. Point FFMPEG at it if it is not on PATH.

import os
import subprocess
import sys

FFMPEG = os.environ.get("FFMPEG", "ffmpeg")


def probe_crop(spec):
    # ffmpeg crop syntax: WxH+X+Y  (also accepts w:h:x:y)
    s = spec.replace(":", "x")
    wh, x, y = s.split("+")
    w, h = wh.split("x")
    return int(w), int(h), int(x), int(y)


def decode_frames(video, w, h, x, y):
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-i", video,
           "-vf", "crop=%d:%d:%d:%d" % (w, h, x, y),
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    need = w * h * 3
    idx = 0
    while True:
        buf = p.stdout.read(need)
        if len(buf) < need:
            break
        yield idx, buf
        idx += 1
    p.stdout.close()
    p.wait()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    video, crop = sys.argv[1], sys.argv[2]
    thresh = 175
    if "--thresh" in sys.argv:
        thresh = int(sys.argv[sys.argv.index("--thresh") + 1])
    out = None
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]

    w, h, x, y = probe_crop(crop)
    print("video  %s" % video)
    print("crop   %dx%d at (%d,%d)  -> %d pixels/frame" % (w, h, x, y, w * h))
    print("thresh %d  (a pixel counts as 'vehicle' when r,g,b are all above this)" % thresh)

    rows = []
    for idx, buf in decode_frames(video, w, h, x, y):
        # stride 2 in both axes: 4x faster and the centroid is unaffected.
        sy = sx = 0
        n = 0
        miny = h
        maxy = -1
        for yy in range(0, h, 2):
            base = yy * w * 3
            for xx in range(0, w, 2):
                o = base + xx * 3
                if buf[o] > thresh and buf[o + 1] > thresh and buf[o + 2] > thresh - 20:
                    sy += yy
                    sx += xx
                    n += 1
                    if yy < miny: miny = yy
                    if yy > maxy: maxy = yy
        if n == 0:
            rows.append((idx, float("nan"), float("nan"), 0, 0, 0))
        else:
            # top / bottom edge of the bright blob, and its vertical extent. A vehicle that
            # bobs vertically moves BOTH edges; a vehicle that merely grows (turns, unfolds)
            # moves them apart. Reporting all three is what makes the reading interpretable.
            rows.append((idx, sx / float(n), sy / float(n), n, miny, maxy))

    good = [r for r in rows if r[3] > 0]
    if len(good) < 8:
        print("FAIL: only %d frames had any vehicle pixels -- widen the crop or lower --thresh"
              % len(good))
        return 1

    # moving-average trend over 7 frames (~0.28 s). Anything shorter than that is what the
    # user perceives as "jitter"; anything longer is just the vehicle driving somewhere.
    K = 3
    def residual(col):
        cy = [r[col] for r in rows]
        out = []
        for i, r in enumerate(rows):
            if r[3] == 0:
                out.append(0.0)
                continue
            lo = max(0, i - K)
            hi = min(len(rows), i + K + 1)
            win = [cy[j] for j in range(lo, hi) if rows[j][3] > 0]
            out.append(cy[i] - sum(win) / len(win))
        return out

    r_cy  = residual(2)
    r_top = residual(4)
    r_bot = residual(5)

    lines = []
    lines.append("  frame   bright     cx      cy     cy-trend    top   bot   extent")
    for i, r in enumerate(rows):
        lines.append("  %5d  %7d  %7.2f  %7.2f  %+8.3f  %5d %5d  %5d"
                     % (r[0], r[3], r[1], r[2], r_cy[i], r[4], r[5],
                        (r[5] - r[4]) if r[3] > 0 else 0))

    def stats(vals, label):
        v = sorted(abs(x) for x in vals if x == x)
        if not v:
            return ["  %-22s (no data)" % label]
        def pct(p):
            k = min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))
            return v[k]
        return ["  %-22s mean %.3f  p50 %.3f  p90 %.3f  p99 %.3f  max %.3f px"
                % (label, sum(v) / len(v), pct(50), pct(90), pct(99), max(v))]

    summary = []
    summary.append("")
    summary.append("================ SUMMARY ================")
    summary.append("frames decoded      %d" % len(rows))
    summary.append("frames with vehicle %d" % len(good))
    summary.append("vehicle pixels/frame %d .. %d (median %d)"
                   % (min(r[3] for r in good), max(r[3] for r in good),
                      sorted(r[3] for r in good)[len(good) // 2]))
    summary.append("")
    summary.append("residual after removing a %d-frame trend:" % (2 * K + 1))
    summary.extend(stats(r_cy, "centroid y"))
    summary.extend(stats(r_top, "top edge y"))
    summary.extend(stats(r_bot, "bottom edge y"))
    summary.append("")
    summary.append("READ IT LIKE THIS:")
    summary.append("  * a vehicle driving in a straight line should give a small mean|r|;")
    summary.append("  * if p90/max are several times the mean, the motion is spiky = jitter;")
    summary.append("  * if top and bottom move TOGETHER, the whole model is bobbing (real jitter);")
    summary.append("  * if only one of them moves, the silhouette is changing (turning / unfolding),")
    summary.append("    which is NOT jitter -- that distinction is the whole point of these columns.")
    summary.append("  * if `bright` jumps a lot, the crop holds more than one vehicle and the")
    summary.append("    centroid is meaningless -- tighten the crop and re-run.")
    summary.append("")
    summary.append("CAVEAT: the recording is 25 fps, so it can only SEE wobble below ~12 Hz;")
    summary.append("a 60 Hz wobble would alias into this band, and the hand-held phone adds its")
    summary.append("own motion. This tool can CONFIRM jitter and measure its size; it cannot")
    summary.append("prove the absence of a fast one.")

    text = "\n".join(summary) + "\n\n" + "\n".join(lines) + "\n"
    print(text)
    if out:
        with open(out, "w", encoding="utf-8") as f:
            f.write(text)
        print("wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
