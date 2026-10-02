# compare_shots.py -- assemble the bisect screenshots into one strip per logic frame.
#
# 2026-09-16 / developer probe
# Why: the bisect captures every config at the SAME absolute logic frames. Looking at 25 separate
# PNGs is both expensive and bad for judging -- differences only pop when the same moment from
# every config sits side by side. One strip per logic frame, configs left to right in run order.
#
# Usage: python compare_shots.py [bisect_dir] [out_dir]
import os
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

def default_bisect_directory():
    repo = Path(__file__).resolve().parents[1]
    build = Path(os.environ.get("FLAB_BUILD_DIR") or repo / "build")
    root = (build if build.is_absolute() else repo / build) / "bisect"
    # New runs get their own directory. Existing direct-layout captures still work.
    runs = [p for p in root.iterdir() if p.is_dir() and (p / "baseline-30").is_dir()] if root.is_dir() else []
    return str(max(runs, key=lambda p: p.name) if runs else root)


ROOT = sys.argv[1] if len(sys.argv) > 1 else default_bisect_directory()
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "_compare")

ORDER = ["baseline-30", "g101", "g103", "g183", "g1FF"]
LABEL_H = 22


def config_sort_key(name):
    return ORDER.index(name) if name in ORDER else len(ORDER)


def main():
    if not os.path.isdir(ROOT):
        print("no such dir:", ROOT)
        return 1
    configs = sorted(
        [d for d in os.listdir(ROOT) if os.path.isdir(os.path.join(ROOT, d)) and not d.startswith("_")],
        key=config_sort_key,
    )
    # target frame -> {config: (path, actual_frame)}
    byframe = {}
    for cfg in configs:
        for fn in os.listdir(os.path.join(ROOT, cfg)):
            m = re.match(r"f(\d+)_at(\d+)\.png$", fn)
            if not m:
                continue
            target, actual = int(m.group(1)), int(m.group(2))
            byframe.setdefault(target, {})[cfg] = (os.path.join(ROOT, cfg, fn), actual)

    if not byframe:
        print("no screenshots found under", ROOT)
        return 1
    os.makedirs(OUT, exist_ok=True)
    print("configs:", ", ".join(configs))

    for target in sorted(byframe):
        shots = byframe[target]
        present = [c for c in configs if c in shots]
        if not present:
            continue
        first = Image.open(shots[present[0]][0])
        w, h = first.size
        strip = Image.new("RGB", (w * len(present), h + LABEL_H), (16, 16, 16))
        draw = ImageDraw.Draw(strip)
        for i, cfg in enumerate(present):
            path, actual = shots[cfg]
            im = Image.open(path).convert("RGB")
            if im.size != (w, h):
                im = im.resize((w, h))
            strip.paste(im, (i * w, LABEL_H))
            # actual frame is printed too: polling latency means it can overshoot the target,
            # and a strip whose panels are not on the same frame is not evidence of anything.
            draw.text((i * w + 6, 5), "%s  logic frame %d" % (cfg, actual), fill=(255, 220, 120))
            if i:
                draw.line([(i * w, 0), (i * w, h + LABEL_H)], fill=(80, 80, 80))
        out = os.path.join(OUT, "frame%04d.png" % target)
        strip.save(out)
        frames = sorted(set(a for _, a in shots.values()))
        print("frame %-5d -> %s   (actual frames captured: %s)" % (target, out, frames))
    return 0


if __name__ == "__main__":
    sys.exit(main())
