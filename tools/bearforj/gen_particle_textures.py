"""Procedural SSPP sprite atlas. No third-party art.

One 2048x2048 atlas, 4x4 cells of 512x512 (512 is the small-sprite size, the
atlas itself is the 2K cap). Uncompressed BGRA8 with a premultiplied mip chain
so the sprites stay soft when they are only a few pixels tall.

Cell order (sprite id = cell + 1, id 0 is the procedural fallback):
  1 rain streak   2 snow flake    3 broad leaf     4 narrow leaf
  5 conifer needle 6 twig         7 spark          8 ember
  9 ash flake     10 ice crumb    11 dust          12 blood drop
  13 water drop   14 pollen       15 butterfly     16 fly
"""
import struct
from pathlib import Path

import numpy as np

CELL = 512
GRID = 4
ATLAS = CELL * GRID
OUT = Path(__file__).resolve().parents[2] / "features" / "Screen Space Physical Particles" / "Textures" / "Bearforj" / "Particles"


def grid():
    y = np.linspace(1.0, -1.0, CELL, dtype=np.float32)
    x = np.linspace(-1.0, 1.0, CELL, dtype=np.float32)
    return np.meshgrid(x, y)


def rgba(rgb, a):
    a = np.clip(a, 0.0, 1.0).astype(np.float32)
    rgb = np.clip(rgb, 0.0, 1.0).astype(np.float32)
    # Kill the square: anything outside the soft falloff is empty.
    rgb = rgb * a[..., None]
    out = np.zeros((CELL, CELL, 4), np.float32)
    out[..., :3] = rgb
    out[..., 3] = a
    return out


def g(x, y, cx, cy, sx, sy):
    return np.exp(-(((x - cx) / sx) ** 2 + ((y - cy) / sy) ** 2))


def rain(x, y):
    core = np.exp(-(x ** 2) / 0.0045) * np.clip(1.0 - np.maximum(-y - 0.92, 0.0) * 8.0, 0.0, 1.0)
    core *= np.clip(1.15 - y * y * 0.35, 0.0, 1.0)
    sheath = np.exp(-(x ** 2) / 0.02) * np.clip(1.0 - y * y * 0.4, 0.0, 1.0) * 0.35
    a = np.clip(core + sheath, 0.0, 1.0)
    col = np.stack([
        np.full_like(a, 0.82),
        np.full_like(a, 0.9),
        np.full_like(a, 1.0),
    ], -1)
    return rgba(col, a)


def snow(x, y):
    r = np.sqrt(x * x + y * y) + 1e-5
    ang = np.arctan2(y, x)
    arm = np.exp(-((ang % (np.pi / 3) - np.pi / 6) ** 2) / 0.012) * np.clip(1.0 - r / 0.82, 0.0, 1.0)
    hub = g(x, y, 0, 0, 0.12, 0.12)
    a = np.clip(arm * 1.4 + hub, 0.0, 1.0)
    return rgba(np.ones((CELL, CELL, 3), np.float32), a)


def leaf(x, y, green, serrate):
    width = 0.34 + 0.22 * np.clip(1.0 - y * y, 0.0, 1.0)
    if serrate:
        width *= 0.72 + 0.08 * np.sin(y * 28.0)
    body = np.clip(1.0 - (x / width) ** 2 - ((y + 0.05) / 0.92) ** 2, 0.0, 1.0)
    body = np.clip(body * 3.0, 0.0, 1.0)
    stem = np.exp(-(x ** 2) / 0.0016) * np.clip(y + 0.95, 0.0, 1.0)
    vein = np.exp(-(x ** 2) / 0.004) * np.clip(0.8 - abs(y), 0.0, 1.0) * 0.45
    a = np.clip(body + stem * 0.8, 0.0, 1.0)
    shade = 0.75 + 0.25 * body
    col = np.stack([
        green[0] * shade * (1.0 - vein * 0.35),
        green[1] * shade * (1.0 - vein * 0.2),
        green[2] * shade,
    ], -1)
    col = np.clip(col + stem[..., None] * 0.15, 0.0, 1.0)
    return rgba(col, a)


def needle(x, y):
    curve = x - 0.18 * y * y
    a = np.exp(-(curve ** 2) / 0.0032) * np.clip(1.0 - (y / 0.96) ** 2, 0.0, 1.0)
    col = np.stack([
        np.full_like(a, 0.22),
        np.full_like(a, 0.42),
        np.full_like(a, 0.18),
    ], -1)
    return rgba(col, np.clip(a * 1.3, 0.0, 1.0))


def twig(x, y):
    main = np.exp(-(x ** 2) / 0.012) * np.clip(1.0 - (y / 0.95) ** 2, 0.0, 1.0)
    bx = (x - 0.28) * 0.85 + (y - 0.15) * 0.5
    by = (y - 0.15) * 0.85 - (x - 0.28) * 0.5
    branch = np.exp(-(bx ** 2) / 0.01) * np.exp(-(by ** 2) / 0.12)
    a = np.clip(main + branch, 0.0, 1.0)
    col = np.stack([
        np.full_like(a, 0.45),
        np.full_like(a, 0.3),
        np.full_like(a, 0.14),
    ], -1)
    return rgba(col, a)


def spark(x, y):
    star = np.exp(-(x ** 2) / 0.004) * np.exp(-(y ** 2) / 0.18) + np.exp(-(y ** 2) / 0.004) * np.exp(-(x ** 2) / 0.18)
    diag = np.exp(-((x - y) ** 2) / 0.01) * np.exp(-((x + y) ** 2) / 0.2)
    core = g(x, y, 0, 0, 0.08, 0.08)
    a = np.clip(star * 1.1 + diag * 0.7 + core, 0.0, 1.0)
    hot = np.clip(core * 2.0, 0.0, 1.0)
    col = np.stack([
        np.ones_like(a),
        0.75 + 0.25 * hot,
        0.35 + 0.65 * hot,
    ], -1)
    return rgba(col, a)


def ember(x, y):
    n = 0.08 * np.sin(x * 11.0 + y * 4.0) + 0.05 * np.sin(x * 5.0 - y * 13.0)
    r = np.sqrt(x * x + y * y)
    a = np.clip(1.0 - r / (0.42 + n), 0.0, 1.0) ** 1.4
    heat = np.clip(1.0 - r / 0.22, 0.0, 1.0)
    col = np.stack([
        np.full_like(a, 1.0),
        0.28 + 0.6 * heat,
        0.05 + 0.35 * heat,
    ], -1)
    return rgba(col, a)


def ash(x, y):
    n = 0.12 * np.sin(x * 9.0) * np.sin(y * 7.0)
    body = np.clip(1.0 - (x / (0.55 + n)) ** 2 - (y / (0.28 + n)) ** 2, 0.0, 1.0)
    a = np.clip(body * 2.2, 0.0, 1.0)
    crack = np.exp(-((y - 0.05 * np.sin(x * 20.0)) ** 2) / 0.004) * a
    col = np.stack([
        0.55 - crack * 0.2,
        0.52 - crack * 0.2,
        0.48 - crack * 0.15,
    ], -1)
    return rgba(np.clip(col, 0.0, 1.0), a)


def ice(x, y):
    d = np.abs(x) * 1.15 + np.abs(y) * 0.55
    a = np.clip(1.15 - d, 0.0, 1.0)
    a = np.clip(a * 4.0, 0.0, 1.0) * np.clip(1.0 - d * 0.15, 0.0, 1.0)
    facet = np.clip(1.0 - np.abs(x) * 3.0 - np.abs(y) * 0.4, 0.0, 1.0)
    col = np.stack([
        0.75 + 0.2 * facet,
        0.9 + 0.1 * facet,
        np.ones_like(a),
    ], -1)
    return rgba(col, a)


def dust(x, y):
    n = 0.15 * np.sin(x * 8.0 + 1.7) * np.cos(y * 6.0)
    a = g(x, y, 0.0, 0.0, 0.55 + n, 0.4) * 0.85
    a += g(x, y, 0.25, 0.1, 0.22, 0.16) * 0.45
    a = np.clip(a, 0.0, 1.0)
    col = np.stack([
        np.full_like(a, 0.78),
        np.full_like(a, 0.7),
        np.full_like(a, 0.55),
    ], -1)
    return rgba(col, a)


def drop(x, y, rgb):
    # Teardrop: round bottom, pulled top.
    y2 = y - 0.15
    stretch = np.where(y2 > 0.0, 0.55 + y2 * 0.9, 1.0)
    r = np.sqrt((x / 0.38) ** 2 + (y2 / (0.42 * stretch)) ** 2)
    a = np.clip(1.15 - r, 0.0, 1.0)
    a = np.clip(a * 5.0, 0.0, 1.0)
    hi = g(x, y, -0.08, 0.28, 0.08, 0.12)
    col = np.zeros((CELL, CELL, 3), np.float32)
    col[..., 0] = rgb[0]
    col[..., 1] = rgb[1]
    col[..., 2] = rgb[2]
    col += hi[..., None] * 0.45
    return rgba(np.clip(col, 0.0, 1.0), a)


def pollen(x, y):
    a = np.zeros((CELL, CELL), np.float32)
    spots = [(0, 0.05, 0.11), (-0.28, 0.18, 0.08), (0.3, 0.12, 0.07),
             (-0.16, -0.26, 0.09), (0.18, -0.22, 0.08), (0.05, 0.32, 0.06),
             (-0.34, -0.05, 0.05)]
    for cx, cy, s in spots:
        a += g(x, y, cx, cy, s, s * 1.3)
    a = np.clip(a, 0.0, 1.0)
    col = np.stack([
        np.full_like(a, 0.95),
        np.full_like(a, 0.82),
        np.full_like(a, 0.28),
    ], -1)
    return rgba(col, a)


def butterfly(x, y):
    wing_l = g(x, y, -0.38, 0.28, 0.32, 0.22) + g(x, y, -0.32, -0.22, 0.26, 0.2)
    wing_r = g(x, y, 0.38, 0.28, 0.32, 0.22) + g(x, y, 0.32, -0.22, 0.26, 0.2)
    body = np.exp(-(x ** 2) / 0.004) * np.exp(-(y ** 2) / 0.18)
    a = np.clip(wing_l + wing_r + body * 1.4, 0.0, 1.0)
    spot = g(x, y, -0.4, 0.3, 0.08, 0.06) + g(x, y, 0.4, 0.3, 0.08, 0.06)
    col = np.stack([
        0.95 * a,
        0.55 * a + 0.3 * spot,
        0.12 * a,
    ], -1)
    col = np.clip(col + body[..., None] * 0.15, 0.0, 1.0)
    return rgba(col / np.maximum(a[..., None], 1e-3), a)


def fly(x, y):
    wing_l = g(x, y, -0.28, 0.12, 0.28, 0.12)
    wing_r = g(x, y, 0.28, 0.12, 0.28, 0.12)
    body = g(x, y, 0.0, -0.08, 0.1, 0.28)
    head = g(x, y, 0.0, 0.32, 0.1, 0.1)
    a = np.clip(wing_l * 0.55 + wing_r * 0.55 + body + head, 0.0, 1.0)
    col = np.stack([
        0.15 + wing_l * 0.5 + wing_r * 0.5,
        0.15 + wing_l * 0.55 + wing_r * 0.55,
        0.18 + wing_l * 0.6 + wing_r * 0.6,
    ], -1)
    col = np.clip(col + (body + head)[..., None] * 0.25, 0.0, 1.0)
    return rgba(col, a)


def build():
    x, y = grid()
    sprites = [
        rain(x, y),
        snow(x, y),
        leaf(x, y, (0.28, 0.62, 0.18), False),
        leaf(x, y, (0.78, 0.62, 0.16), True),
        needle(x, y),
        twig(x, y),
        spark(x, y),
        ember(x, y),
        ash(x, y),
        ice(x, y),
        dust(x, y),
        drop(x, y, (0.72, 0.05, 0.04)),
        drop(x, y, (0.72, 0.86, 1.0)),
        pollen(x, y),
        butterfly(x, y),
        fly(x, y),
    ]
    atlas = np.zeros((ATLAS, ATLAS, 4), np.float32)
    for i, spr in enumerate(sprites):
        cover = float((spr[..., 3] > 0.2).mean())
        if cover < 0.01:
            raise SystemExit(f"sprite {i + 1} is empty")
        row, col = divmod(i, GRID)
        # divmod with row-major: i = row * GRID + col, row 0 at top
        r, c = i // GRID, i % GRID
        atlas[r * CELL:(r + 1) * CELL, c * CELL:(c + 1) * CELL] = spr
        print(f"sprite {i + 1:2d} coverage {cover:.3f}")
    return atlas


def downsample(img):
    h, w, _ = img.shape
    prem = img.copy()
    prem[..., :3] *= img[..., 3:4]
    ds = prem.reshape(h // 2, 2, w // 2, 2, 4).mean(axis=(1, 3))
    a = ds[..., 3:4]
    rgb = ds[..., :3] / np.maximum(a, 1e-4)
    out = np.concatenate([np.clip(rgb, 0.0, 1.0), np.clip(a, 0.0, 1.0)], -1)
    out[a[..., 0] < 1e-4] = 0.0
    return out.astype(np.float32)


def to_bgra(img):
    bgra = np.empty(img.shape, np.uint8)
    bgra[..., 0] = np.clip(np.rint(img[..., 2] * 255.0), 0, 255)
    bgra[..., 1] = np.clip(np.rint(img[..., 1] * 255.0), 0, 255)
    bgra[..., 2] = np.clip(np.rint(img[..., 0] * 255.0), 0, 255)
    bgra[..., 3] = np.clip(np.rint(img[..., 3] * 255.0), 0, 255)
    return bgra


def write_dds(path, base):
    levels = [base]
    cur = base
    while min(cur.shape[0], cur.shape[1]) > 1:
        cur = downsample(cur)
        levels.append(cur)
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000
    h, w = base.shape[:2]
    header = struct.pack(
        "<4s7I11I8I5I",
        b"DDS ",
        124,
        flags,
        h,
        w,
        w * 4,
        0,
        len(levels),
        *([0] * 11),
        32,
        0x41,
        0,
        32,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
        0x401008,
        0,
        0,
        0,
        0,
    )
    assert len(header) == 128
    blob = bytearray(header)
    for level in levels:
        blob += to_bgra(level).tobytes()
    path.write_bytes(blob)
    print(path, len(blob), "mips", len(levels))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    write_dds(OUT / "atlas.dds", build())
    (OUT / "SOURCES.txt").write_text(
        "Bearforj particle sprites, generated by tools/bearforj/gen_particle_textures.py.\n"
        "Procedural. No third-party images, no photographed textures.\n"
        "atlas.dds is 2048x2048, sixteen 512x512 cells, uncompressed BGRA8 with mipmaps.\n"
        "Sprite 0 is unused here: SSPP draws a procedural quad when the atlas is missing\n"
        "or the particle has no sprite id.\n"
        "Ids: 1 rain, 2 snow, 3 broad leaf, 4 narrow leaf, 5 needle, 6 twig, 7 spark,\n"
        "8 ember, 9 ash, 10 ice, 11 dust, 12 blood drop, 13 water drop, 14 pollen,\n"
        "15 butterfly, 16 fly.\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
