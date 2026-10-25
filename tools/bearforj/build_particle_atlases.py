#!/usr/bin/env python3
"""Pack the Bearforj particle PNGs into the three 2048 DXT5 atlases particles.json names.

Sources are 2048 square. The loader's cells are 512 (leaves, small) or 1024 (sparks),
so each image is integer-box downscaled. Downscale is premultiplied so straight-alpha
edges do not pick up a dark fringe, then unpremultiplied before BC3. Mips are built
per cell so a mip does not blend across cell borders until a cell is smaller than one
pixel. The DDS is legacy DXT5 FourCC, not a DX10 header.
"""

from __future__ import annotations

import json
import struct
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "features" / "Screen Space Physical Particles" / "Shaders" / "PhysicalParticles"
LEAVES_ZIP = Path("/home/ubuntu/.cursor/projects/workspace/uploads/tex_leaves_0631.zip")
O1_ZIP = Path("/home/ubuntu/.cursor/projects/workspace/uploads/tex_o1_0d8d.zip")
O2_ZIP = Path("/home/ubuntu/.cursor/projects/workspace/uploads/tex_o2_15d3.zip")
ATLAS = 2048


def load_pngs(zip_path: Path) -> dict[str, np.ndarray]:
    images: dict[str, np.ndarray] = {}
    with zipfile.ZipFile(zip_path) as archive:
        for name in archive.namelist():
            if not name.lower().endswith(".png"):
                continue
            with archive.open(name) as handle:
                image = Image.open(handle).convert("RGBA")
            array = np.asarray(image, dtype=np.uint8)
            if array.shape[0] != ATLAS or array.shape[1] != ATLAS:
                raise SystemExit(f"{name} is {array.shape}, expected {ATLAS}x{ATLAS}")
            images[Path(name).name] = array
    return images


def premultiply(image: np.ndarray) -> np.ndarray:
    rgba = image.astype(np.float32)
    alpha = rgba[..., 3:4] / 255.0
    rgba[..., :3] *= alpha
    return rgba


def unpremultiply(image: np.ndarray) -> np.ndarray:
    rgba = image.copy()
    alpha = np.clip(rgba[..., 3:4], 0.0, 255.0)
    mask = alpha > 0.0
    rgb = np.zeros_like(rgba[..., :3])
    np.divide(rgba[..., :3], alpha / 255.0, out=rgb, where=mask)
    rgba[..., :3] = np.clip(rgb, 0.0, 255.0)
    return rgba


def box_down(image: np.ndarray, factor: int) -> np.ndarray:
    if factor == 1:
        return image
    height, width, channels = image.shape
    if height % factor or width % factor:
        raise SystemExit(f"cannot box-down {width}x{height} by {factor}")
    return image.reshape(height // factor, factor, width // factor, factor, channels).mean(axis=(1, 3))


def to_cell(source: np.ndarray, cell: int) -> np.ndarray:
    factor = source.shape[0] // cell
    return unpremultiply(box_down(premultiply(source), factor))


def quantize_565(color: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    red = np.clip(np.rint(color[..., 0] * (31.0 / 255.0)), 0, 31).astype(np.uint16)
    green = np.clip(np.rint(color[..., 1] * (63.0 / 255.0)), 0, 63).astype(np.uint16)
    blue = np.clip(np.rint(color[..., 2] * (31.0 / 255.0)), 0, 31).astype(np.uint16)
    packed = (red << 11) | (green << 5) | blue
    expanded = np.empty(color.shape, np.float32)
    expanded[..., 0] = (red.astype(np.float32) * 255.0) / 31.0
    expanded[..., 1] = (green.astype(np.float32) * 255.0) / 63.0
    expanded[..., 2] = (blue.astype(np.float32) * 255.0) / 31.0
    return packed, expanded


def encode_bc3(image: np.ndarray) -> bytes:
    height, width, _ = image.shape
    padded_h = max(4, (height + 3) & ~3)
    padded_w = max(4, (width + 3) & ~3)
    if padded_h != height or padded_w != width:
        padded = np.zeros((padded_h, padded_w, 4), np.uint8)
        padded[:height, :width] = image
        if height < 4:
            padded[height:, :width] = image[-1:, :]
        if width < 4:
            padded[:height, width:] = image[:, -1:]
        image = padded
        height, width = padded_h, padded_w
    blocks_y = height // 4
    blocks_x = width // 4
    pixels = image.reshape(blocks_y, 4, blocks_x, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    count = pixels.shape[0]
    output = np.empty((count, 16), np.uint8)
    chunk = 16384
    for start in range(0, count, chunk):
        end = min(start + chunk, count)
        block = pixels[start:end]
        number = end - start
        color = block[..., :3].astype(np.float32)
        luminance = color[..., 0] * 0.299 + color[..., 1] * 0.587 + color[..., 2] * 0.114
        index_max = np.argmax(luminance, axis=1)
        index_min = np.argmin(luminance, axis=1)
        rows = np.arange(number)
        endpoint_max = color[rows, index_max]
        endpoint_min = color[rows, index_min]
        inset = (endpoint_max - endpoint_min) / 16.0
        endpoint_max = np.clip(endpoint_max - inset, 0.0, 255.0)
        endpoint_min = np.clip(endpoint_min + inset, 0.0, 255.0)
        packed_hi, decoded_hi = quantize_565(endpoint_max)
        packed_lo, decoded_lo = quantize_565(endpoint_min)
        swap = packed_hi < packed_lo
        color0 = np.where(swap, packed_lo, packed_hi).astype(np.uint16)
        color1 = np.where(swap, packed_hi, packed_lo).astype(np.uint16)
        decoded0 = np.where(swap[..., None], decoded_lo, decoded_hi)
        decoded1 = np.where(swap[..., None], decoded_hi, decoded_lo)
        # 4-color mode needs color0 > color1. A flat block steps the low endpoint down.
        equal = color0 <= color1
        color1 = np.where(equal & (color1 > 0), color1 - 1, color1).astype(np.uint16)
        color0 = np.where(color0 <= color1, np.minimum(color1 + 1, np.uint16(65535)), color0).astype(np.uint16)
        palette = np.empty((number, 4, 3), np.float32)
        palette[:, 0] = decoded0
        palette[:, 1] = decoded1
        palette[:, 2] = (2.0 * decoded0 + decoded1) / 3.0
        palette[:, 3] = (decoded0 + 2.0 * decoded1) / 3.0
        distance = np.sum((color[:, :, None, :] - palette[:, None, :, :]) ** 2, axis=3)
        color_index = np.argmin(distance, axis=2).astype(np.uint32)
        color_bits = np.zeros(number, np.uint32)
        for texel in range(16):
            color_bits |= color_index[:, texel] << np.uint32(2 * texel)
        output[start:end, 8] = color0 & 0xFF
        output[start:end, 9] = color0 >> 8
        output[start:end, 10] = color1 & 0xFF
        output[start:end, 11] = color1 >> 8
        output[start:end, 12] = color_bits & 0xFF
        output[start:end, 13] = (color_bits >> 8) & 0xFF
        output[start:end, 14] = (color_bits >> 16) & 0xFF
        output[start:end, 15] = (color_bits >> 24) & 0xFF

        alpha = block[..., 3].astype(np.int16)
        alpha0 = alpha.max(axis=1).astype(np.int16)
        alpha1 = alpha.min(axis=1).astype(np.int16)
        equal_alpha = alpha0 == alpha1
        alpha0 = np.where(equal_alpha & (alpha0 < 255), alpha0 + 1, alpha0)
        alpha1 = np.where(equal_alpha & (alpha0 == 255), 254, alpha1)
        alpha0 = alpha0.astype(np.int16)
        alpha1 = alpha1.astype(np.int16)
        # 8-value palette requires alpha0 > alpha1.
        still_low = alpha0 <= alpha1
        alpha0 = np.where(still_low, np.minimum(alpha1 + 1, 255), alpha0)
        alpha1 = np.where(still_low & (alpha0 == alpha1), np.maximum(alpha0 - 1, 0), alpha1)
        palette_a = np.empty((number, 8), np.float32)
        palette_a[:, 0] = alpha0
        palette_a[:, 1] = alpha1
        for step in range(1, 7):
            palette_a[:, step + 1] = ((7 - step) * alpha0.astype(np.float32) + step * alpha1.astype(np.float32)) / 7.0
        alpha_distance = np.abs(alpha[:, :, None].astype(np.float32) - palette_a[:, None, :])
        alpha_index = np.argmin(alpha_distance, axis=2).astype(np.uint64)
        alpha_bits = np.zeros(number, np.uint64)
        for texel in range(16):
            alpha_bits |= alpha_index[:, texel] << np.uint64(3 * texel)
        output[start:end, 0] = alpha0.astype(np.uint8)
        output[start:end, 1] = alpha1.astype(np.uint8)
        for byte in range(6):
            output[start:end, 2 + byte] = (alpha_bits >> np.uint64(8 * byte)) & np.uint64(0xFF)
    return output.tobytes()


def write_dds(path: Path, mips: list[np.ndarray]) -> None:
    payload = b"".join(encode_bc3(mip) for mip in mips)
    height, width, _ = mips[0].shape
    linear = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * 16
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
    caps = 0x1000 | (0x8 | 0x400000 if len(mips) > 1 else 0)
    header = struct.pack("<I", 0x20534444)
    header += struct.pack("<7I", 124, flags, height, width, linear, 0, len(mips))
    header += b"\x00" * 44
    header += struct.pack("<II", 32, 0x4)
    header += b"DXT5"
    header += struct.pack("<5I", 0, 0, 0, 0, 0)
    header += struct.pack("<5I", caps, 0, 0, 0, 0)
    if len(header) != 128:
        raise SystemExit(f"DDS header is {len(header)} bytes")
    path.write_bytes(header + payload)


def build_mips(cells: list[np.ndarray], grid: int) -> list[np.ndarray]:
    """cells are premultiplied float images at the base cell size."""
    mips: list[np.ndarray] = []
    level_cells = cells
    size = ATLAS
    while size >= 1:
        cell = size // grid
        if cell >= 1:
            canvas = np.zeros((size, size, 4), np.float32)
            for index, image in enumerate(level_cells):
                if image.shape[0] != cell:
                    raise SystemExit(f"cell {index} is {image.shape[0]}, expected {cell}")
                y = (index // grid) * cell
                x = (index % grid) * cell
                canvas[y : y + cell, x : x + cell] = image
            mips.append(np.clip(np.rint(unpremultiply(canvas)), 0, 255).astype(np.uint8))
            if cell == 1:
                level_cells = []
            else:
                level_cells = [box_down(image, 2) for image in level_cells]
        else:
            mips.append(np.clip(np.rint(unpremultiply(box_down(premultiply(mips[-1]), 2))), 0, 255).astype(np.uint8))
        size //= 2
    return mips


def cells_from(images: dict[str, np.ndarray], names: list[str], cell: int) -> list[np.ndarray]:
    prepared = []
    for name in names:
        if name not in images:
            raise SystemExit(f"missing {name}")
        prepared.append(premultiply(np.clip(np.rint(to_cell(images[name], cell)), 0, 255).astype(np.uint8)))
    return prepared


def main() -> None:
    leaves = load_pngs(LEAVES_ZIP)
    other = load_pngs(O1_ZIP)
    other.update(load_pngs(O2_ZIP))
    document = json.loads((OUT_DIR / "particles.json").read_text())
    leaf_names = [entry["id"] + ".png" for entry in document["leaves"] if entry["atlas"] == "leaves"]
    spark_names = [entry["id"] + ".png" for entry in document["sparks"]]
    small_names = [""] * 16
    for group in ("fluff", "spores", "fireflies", "moths", "dandelions"):
        for entry in document[group]:
            small_names[entry["cell"]] = entry["id"] + ".png"
    for entry in document["leaves"]:
        if entry["atlas"] == "small":
            small_names[entry["cell"]] = entry["id"] + ".png"
    if any(not name for name in small_names):
        raise SystemExit(f"small atlas has empty cells: {small_names}")

    jobs = (
        ("leaves_atlas.dds", cells_from(leaves, leaf_names, 512), 4),
        ("sparks_atlas.dds", cells_from(other, spark_names, 1024), 2),
        ("small_atlas.dds", cells_from(other, small_names, 512), 4),
    )
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for filename, cells, grid in jobs:
        print(f"packing {filename}", flush=True)
        mips = build_mips(cells, grid)
        destination = OUT_DIR / filename
        write_dds(destination, mips)
        print(f"  wrote {destination} ({destination.stat().st_size} bytes, {len(mips)} mips)", flush=True)


if __name__ == "__main__":
    main()
