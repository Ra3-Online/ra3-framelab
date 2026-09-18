# diff_shots.py -- quantify how much each config's frame differs from the baseline at the SAME logic frame.
#
# 2026-09-16 / session 68ee9b9d (Claude)
# At a given absolute logic frame the simulation state is identical across configs (same replay,
# lockstep). So any pixel difference comes from rendering: interpolation phase, or an animation
# that is being advanced at the wrong rate. A small difference is expected (a 90 fps config lands
# on a different sub-frame than a 30 fps one); a LARGE, structured difference is the signal.
#
# Usage: python diff_shots.py [bisect_dir]
import os
import re
import sys

from PIL import Image, ImageChops

ROOT = sys.argv[1] if len(sys.argv) > 1 else r"G:\Ra3 FrameLab\build\bisect"
BASE = "baseline-30"


def load(cfg, target):
    d = os.path.join(ROOT, cfg)
    if not os.path.isdir(d):
        return None
    for fn in os.listdir(d):
        m = re.match(r"f(\d+)_at(\d+)\.png$", fn)
        if m and int(m.group(1)) == target:
            return Image.open(os.path.join(d, fn)).convert("RGB")
    return None


def looks_like_game(im):
    """A capture can grab whatever covered the window. Judge the image by its content, not by a
    window flag: a real 3D scene has hundreds of distinct colours, a desktop/UI grab has a few dozen."""
    px = list(im.getdata())
    return len(set(px[::499])) > 60


def metric(a, b):
    if a.size != b.size:
        b = b.resize(a.size)
    diff = ImageChops.difference(a, b)
    px = list(diff.getdata())
    n = len(px)
    mean = sum(r + g + bb for r, g, bb in px) / (3.0 * n)
    big = sum(1 for r, g, bb in px if max(r, g, bb) > 32)
    return mean, 100.0 * big / n


def main():
    cfgs = sorted(d for d in os.listdir(ROOT)
                  if os.path.isdir(os.path.join(ROOT, d)) and not d.startswith("_") and d != BASE)
    targets = sorted({int(re.match(r"f(\d+)_", f).group(1))
                      for f in os.listdir(os.path.join(ROOT, BASE)) if re.match(r"f\d+_", f)})
    print("baseline = %s" % BASE)
    print("%-10s %-12s %-12s %s" % ("frame", "config", "mean|diff|", "pixels differing >32"))
    for t in targets:
        b = load(BASE, t)
        if b is None:
            continue
        if not looks_like_game(b):
            print("%-10d %-12s baseline capture is not game content - frame skipped" % (t, BASE))
            continue
        for c in cfgs:
            o = load(c, t)
            if o is None:
                print("%-10d %-12s (missing)" % (t, c))
                continue
            if not looks_like_game(o):
                print("%-10d %-12s SKIPPED - capture is not game content, NOT evidence" % (t, c))
                continue
            mean, pct = metric(b, o)
            print("%-10d %-12s %-12.2f %.1f%%" % (t, c, mean, pct))
    print()
    print("note: identical sim state at the same logic frame, so this is purely a rendering difference.")
    print("      a config rendering at 90 fps lands on a different interpolation sub-step than the 30 fps")
    print("      baseline, so a few percent is normal; a big jump is the animation signal.")


if __name__ == "__main__":
    main()
