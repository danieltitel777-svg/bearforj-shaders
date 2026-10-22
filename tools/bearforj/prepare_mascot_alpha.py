#!/usr/bin/env python3
"""Copy Bearforj UI art into package/ and keep a real alpha channel.

Logo: the supplied file is copied byte-for-byte (no crop, no resize).
Emotions: copied byte-for-byte when they already have transparent pixels.
An opaque white backdrop is turned into alpha by a flood fill from the image
edges only, so white fur inside the character stays opaque. Output is straight
(not premultiplied) RGBA, which matches ImGui's SRC_ALPHA / INV_SRC_ALPHA blend.
"""
import shutil
import sys
from collections import deque
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / "package" / "Interface" / "CommunityShaders" / "Bearforj"
# Silhouette order of uploads/ui_fix/emotion_*.png (sorted by filename).
EMOTIONS = [
    "proud",       # 00 standing, tongue
    "wink",        # 01
    "surprised",   # 02
    "wrench",      # 03 tool in the left hand
    "thumbs_up",   # 04
    "singing",     # 05
    "hourglass",   # 06
    "sword",       # 07
    "idea",        # 08 bulb above the helmet
    "waving",      # 09
    "sleeping",    # 10
    "thinking",    # 11
    "pointing",    # 12
    "cheering",    # 13 both arms up
    "bug",         # 14
]


def has_alpha(path: Path) -> bool:
    with Image.open(path) as im:
        if im.mode in ("RGBA", "LA"):
            return im.getchannel("A").getextrema()[0] < 250
        return False


def knock_edge_white(src: Path, dest: Path, tol: int = 18) -> None:
    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size
    seen = bytearray(w * h)
    q = deque()

    def consider(x, y):
        i = y * w + x
        if seen[i]:
            return
        r, g, b, a = px[x, y]
        seen[i] = 1
        if a < 250 or (r >= 255 - tol and g >= 255 - tol and b >= 255 - tol):
            q.append((x, y))

    for x in range(w):
        consider(x, 0)
        consider(x, h - 1)
    for y in range(h):
        consider(0, y)
        consider(w - 1, y)
    while q:
        x, y = q.popleft()
        r, g, b, a = px[x, y]
        if a >= 250 and r >= 255 - tol and g >= 255 - tol and b >= 255 - tol:
            px[x, y] = (r, g, b, 0)
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h:
                consider(nx, ny)
    dest.parent.mkdir(parents=True, exist_ok=True)
    im.save(dest, "PNG")


def place(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if has_alpha(src):
        shutil.copy2(src, dest)
    else:
        knock_edge_white(src, dest)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: prepare_mascot_alpha.py <ui_fix dir>", file=sys.stderr)
        return 2
    src = Path(sys.argv[1])
    logo = src / "LOGO_full_helmet.png"
    if not logo.is_file():
        print("missing", logo, file=sys.stderr)
        return 1
    emotions = sorted(src.glob("emotion_*.png"))
    if len(emotions) != len(EMOTIONS):
        print(f"expected {len(EMOTIONS)} emotion pngs, found {len(emotions)}", file=sys.stderr)
        return 1
    place(logo, DEST / "logo.png")
    place(logo, DEST / "brand" / "LOGO_full_helmet.png")
    out_dir = DEST / "emotions"
    for name, path in zip(EMOTIONS, emotions):
        place(path, out_dir / f"{name}.png")
    # These were tight crops with an opaque white field. The menu no longer loads them.
    for stale in (
        "mishmish_front.png",
        "head_happy.png",
        "head_grumpy.png",
        "head_worried.png",
        "emotions/happy.png",
    ):
        p = DEST / stale
        if p.is_file():
            p.unlink()
    print(f"logo {logo.stat().st_size} bytes, {len(emotions)} emotions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
