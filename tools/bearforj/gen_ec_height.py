#!/usr/bin/env python3
"""Height maps for Elemental Contact water. EC v0.13.2 ships foam.dds and
water_normals.dds and no height maps.

foam_height.dds is the foam.dds alpha (BC3), the lace the shader already uses.
wave_height.dds integrates the water_normals.dds slopes (BC5 / ATI2) in the
Fourier domain, the same 2048 tile. Both are R8_UNORM with a full mip chain.

Usage: python tools/bearforj/gen_ec_height.py <foam.dds> <water_normals.dds> <out dir>
"""
import struct
import sys

import numpy as np


def bc4_decode(blocks):
    a0 = blocks[:, 0].astype(np.int16)
    a1 = blocks[:, 1].astype(np.int16)
    bits = np.zeros(blocks.shape[0], np.uint64)
    for i in range(6):
        bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.empty((blocks.shape[0], 16), np.uint8)
    for p in range(16):
        idx[:, p] = (bits >> np.uint64(3 * p)) & np.uint64(7)
    pal = np.zeros((blocks.shape[0], 8), np.float32)
    hi = a0 > a1
    pal[:, 0] = a0
    pal[:, 1] = a1
    for i in range(1, 7):
        pal[hi, i + 1] = ((7 - i) * a0[hi] + i * a1[hi]) / 7.0
    pal[~hi, 2] = (4 * a0[~hi] + 1 * a1[~hi]) / 5.0
    pal[~hi, 3] = (3 * a0[~hi] + 2 * a1[~hi]) / 5.0
    pal[~hi, 4] = (2 * a0[~hi] + 3 * a1[~hi]) / 5.0
    pal[~hi, 5] = (1 * a0[~hi] + 4 * a1[~hi]) / 5.0
    pal[~hi, 6] = 0.0
    pal[~hi, 7] = 255.0
    return np.take_along_axis(pal, idx.astype(np.intp), axis=1) / 255.0


def blocks_to_image(pix, size):
    bw = size // 4
    return pix.reshape(size // 4, bw, 4, 4).transpose(0, 2, 1, 3).reshape(size, size)


def mip0_blocks(data, size, block_bytes):
    count = (size // 4) * (size // 4)
    return np.frombuffer(data, np.uint8, count=count * block_bytes, offset=128).reshape(count, block_bytes)


def integrate_slopes(nx, ny):
    # slope = ifft(1j * k * fft(height)), so height comes back from both slopes.
    fx = np.fft.fft2(nx)
    fy = np.fft.fft2(ny)
    k = np.fft.fftfreq(nx.shape[0]) * (2.0 * np.pi)
    kx, ky = np.meshgrid(k, k)
    denom = kx * kx + ky * ky
    denom[0, 0] = 1.0
    height = (kx * fx + ky * fy) / (1j * denom)
    height[0, 0] = 0.0
    return np.real(np.fft.ifft2(height)).astype(np.float32)


def mips(img):
    out = [img]
    cur = img
    while cur.shape[0] > 1:
        cur = (0.25 * (cur[0::2, 0::2] + cur[1::2, 0::2] + cur[0::2, 1::2] + cur[1::2, 1::2])).astype(np.float32)
        out.append(cur)
    return out


def write_r8(path, img):
    levels = mips(img)
    height, width = levels[0].shape
    header = bytearray(128)
    header[0:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x2100F, height, width, width, 0, len(levels))
    struct.pack_into("<2I4s5I", header, 76, 32, 0x4, b"DX10", 0, 0, 0, 0, 0)
    struct.pack_into("<5I", header, 108, 0x401008, 0, 0, 0, 0)
    dx10 = struct.pack("<5I", 61, 3, 0, 1, 0)
    body = b"".join(np.clip(np.rint(level * 255.0), 0, 255).astype(np.uint8).tobytes() for level in levels)
    with open(path, "wb") as out:
        out.write(bytes(header) + dx10 + body)


def main():
    foam_path, normal_path, out_dir = sys.argv[1:4]
    foam = open(foam_path, "rb").read()
    normals = open(normal_path, "rb").read()
    if foam[84:88] != b"DXT5" or normals[84:88] != b"ATI2":
        raise SystemExit("expected foam DXT5 and water_normals ATI2")
    foam_h = blocks_to_image(bc4_decode(mip0_blocks(foam, 2048, 16)[:, :8]), 2048).astype(np.float32)
    red = blocks_to_image(bc4_decode(mip0_blocks(normals, 2048, 16)[:, :8]), 2048)
    green = blocks_to_image(bc4_decode(mip0_blocks(normals, 2048, 16)[:, 8:]), 2048)
    wave = integrate_slopes((red * 2.0 - 1.0).astype(np.float32), (green * 2.0 - 1.0).astype(np.float32))
    lo, hi = np.percentile(wave, [1.0, 99.0])
    wave = np.clip((wave - lo) / max(float(hi - lo), 1e-6), 0.0, 1.0).astype(np.float32)
    write_r8(out_dir + "/foam_height.dds", foam_h)
    write_r8(out_dir + "/wave_height.dds", wave)
    print("foam alpha mean", float(foam_h.mean()), "wave mean", float(wave.mean()), "wave std", float(wave.std()))


if __name__ == "__main__":
    main()
