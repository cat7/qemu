#!/usr/bin/env python3
"""qtest wall-clock benchmark for the Rage 128 2D/3D engine.

Same harness base as raster_regress.py. One frame's worth of CCE
packets is written into VRAM once as an indirect buffer and dispatched
N times through PM4_IW_INDOFF/INDSIZE (the path Nanosaur's driver
uses), so the qtest socket costs two register writes per frame and
the wall clock measures the engine. A frame is Nanosaur's shape:

  PAINT_MULTI clear of the 640x480 Z buffer and colour back buffer
  (corpus GMC 0x12f033da), the _C context block re-programmed, 100
  textured GEN_PRIM triangles (vc_format 0xa7, 256x256 ARGB1555
  bilinear-modulated, Z on) + 100 Gouraud ones (vc_format 0x07), each
  a 64x64 right triangle = 2016 px, then a BITBLT_MULTI back->front
  present (corpus GMC 0x52cc33ff).

Prints seconds, frames/s and Mpixel/s, plus a CRC of the front buffer
after the run (deterministic: the same binary and frame count must
give the same CRC, and a rewrite that changes no rendered byte gives
the same CRC as the build before it).

Run from the tree root:  python3 doc/rage128-3d/raster_bench.py [frames]
"""

import os
import struct
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import raster_regress as rr  # noqa: E402

FRONT = 0x100000
BACK = 0x0F0300 << 5           # corpus DST_PITCH_OFFSET_C offset
ZBUF = 0x1C00000
TEX = 0x01D0AE00               # corpus PRIM_TEX_8_OFFSET_C
IB = 0x400000
W, H = 640, 480
PITCH = 0x50                   # 640 px in units of 8

PM4_IW_INDOFF = 0x0738
PM4_IW_INDSIZE = 0x073C


def po(offset):
    return PITCH << 21 | offset >> 5


def packet3(opcode, payload):
    return [0xC0000000 | (len(payload) - 1) << 16 | opcode << 8] + payload


def reg(offset, val):
    return [offset >> 2, val]


def build_frame():
    ib = []
    for dst, colour in ((ZBUF, 0xFFFFFFFF), (BACK, 0x7BD87BD8)):
        ib += packet3(0x9A, [0x12F033DA, po(dst), 0, (H - 1) << 16 | (W - 1),
                             colour, 0, W << 16 | H])
    ib += reg(rr.DST_PITCH_OFFSET_C, po(BACK))
    ib += reg(rr.DP_GUI_MASTER_CNTL_C, rr.GMC_ARGB1555)
    ib += reg(rr.SC_TOP_LEFT_C, 0)
    ib += reg(rr.SC_BOTTOM_RIGHT_C, (H - 1) << 16 | (W - 1))
    ib += reg(rr.Z_OFFSET_C, ZBUF)
    ib += reg(rr.Z_PITCH_C, 0x00010050)
    ib += reg(rr.Z_STEN_CNTL_C, 0x10)
    ib += reg(rr.MISC_3D_STATE_CNTL_REG, 0x00510200)
    ib += reg(rr.PRIM_TEX_CNTL_C, 0x010300B6)
    ib += reg(rr.PRIM_TEXTURE_COMBINE_CNTL_C, 0x0418D043)
    ib += reg(rr.TEX_SIZE_PITCH_C, 0x03330888)
    ib += reg(rr.PRIM_TEX_0_OFFSET_C + 4 * 8, 0xC0000000 | TEX)
    ib += reg(rr.PLANE_3D_MASK_C, 0xFFFFFFFF)
    seed = 12345
    tris = []
    for i in range(200):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        x = 8 + (seed >> 8) % (W - 80)
        y = 8 + (seed >> 20) % (H - 80)
        tris.append((float(x), float(y), 0.3 + (i % 7) * 0.1,
                     ((i % 5) / 4.0, (i % 3) / 2.0, (i % 2), 1.0)))
    ib += reg(rr.TEX_CNTL_C, 0x193)
    for x, y, z, col in tris[:100]:
        pl = [0xA7, 3 << 16 | 0x34]
        for (vx, vy, s, t) in ((x, y, 0.0, 0.0), (x + 64, y, 1.0, 0.0),
                               (x, y + 64, 0.0, 1.0)):
            r, g, b, a = col
            pl += [rr.f32(vx), rr.f32(vy), rr.f32(z), rr.f32(1.0),
                   rr.f32(b), rr.f32(g), rr.f32(r), rr.f32(a), rr.f32(0.0),
                   rr.f32(s), rr.f32(t)]
        ib += packet3(0x25, pl)
    ib += reg(rr.TEX_CNTL_C, 0x183)
    for x, y, z, col in tris[100:]:
        pl = [0x07, 3 << 16 | 0x34]
        for (vx, vy) in ((x, y), (x + 64, y), (x, y + 64)):
            r, g, b, a = col
            pl += [rr.f32(vx), rr.f32(vy), rr.f32(z), rr.f32(1.0),
                   rr.f32(b), rr.f32(g), rr.f32(r), rr.f32(a)]
        ib += packet3(0x25, pl)
    ib += packet3(0x9B, [0x52CC33FF, po(BACK), po(FRONT),
                         (H - 1) << 16 | (W - 1), 0, (H - 1) << 16 | (W - 1),
                         0, 0, W << 16 | H])
    return ib


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    os.makedirs(rr.SCRATCH, exist_ok=True)
    sock_dir = tempfile.mkdtemp(prefix="r128qb-")
    sock_path = os.path.join(sock_dir, "qtest.sock")
    q = rr.QTest(sock_path)
    try:
        c = rr.Card(q)
        tex = b"".join(struct.pack("<H", 0x8000 | (x >> 3) << 10 |
                                   (y >> 3) << 5 | ((x ^ y) >> 3))
                       for y in range(256) for x in range(256))
        q.b64write(rr.BAR0 + TEX, tex)
        q.memset(rr.BAR0 + FRONT, W * H * 2, 0)
        ib = build_frame()
        q.b64write(rr.BAR0 + IB, struct.pack("<%dI" % len(ib), *ib))
        c.reg(rr.DP_CNTL, 0x3)
        # one warm-up frame, checked, outside the timed window
        q.wl(rr.BAR2 + PM4_IW_INDOFF, IB)
        q.wl(rr.BAR2 + PM4_IW_INDSIZE, len(ib))
        front = q.b64read(rr.BAR0 + FRONT, W * H * 2)
        painted = rr.count16(front)
        if painted != W * H:
            sys.exit(f"warm-up frame painted {painted} px, want {W * H}")
        t0 = time.perf_counter()
        for _ in range(frames):
            q.wl(rr.BAR2 + PM4_IW_INDOFF, IB)
            q.wl(rr.BAR2 + PM4_IW_INDSIZE, len(ib))
        dt = time.perf_counter() - t0
        front = q.b64read(rr.BAR0 + FRONT, W * H * 2)
        crc = zlib.crc32(front) & 0xFFFFFFFF
        px_per_frame = 3 * W * H + 200 * 2016   # 2 clears + present + tris
        print(f"{frames} frames of {len(ib)} IB dwords: {dt:.3f} s, "
              f"{frames / dt:.1f} frames/s, "
              f"{frames * px_per_frame / dt / 1e6:.1f} Mpx/s "
              f"({px_per_frame} px/frame); front buffer crc 0x{crc:08x}")
    finally:
        q.close()
        if os.path.exists(sock_path):
            os.unlink(sock_path)
        os.rmdir(sock_dir)


if __name__ == "__main__":
    main()
