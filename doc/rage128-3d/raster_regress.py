#!/usr/bin/env python3
"""qtest regression harness for the Rage 128 3D software rasterizer.

Boots g3beige under -accel qtest (no guest code runs), finds the
ati-rage128-pro on the Grackle host bridge, programs BARs, and drives
the CCE PIO FIFO (BAR2 + 0x1000) with packet0 state writes and
GEN_PRIM packet3s -- the same path Nanosaur's RAVE driver uses.
Pixels are read back raw through the BAR0 VRAM aperture.

Six checks (see HANDOFF-rasterizer.md): known-answer triangle,
Gouraud interpolation, Z test (+ negative control), scissor negative
control, corpus replay, and 2D-unharmed.

Run from the tree root:  python3 doc/rage128-3d/raster_regress.py
"""

import base64
import gzip
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
QEMU = os.path.join(TREE, "build-g3", "qemu-system-ppc")
CORPUS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "nanosaur-payload-sample.log.gz")
SCRATCH = os.environ.get(
    "RASTER_SCRATCH",
    "/private/tmp/claude-502/-Users-hsp-src-claude-code-qemu-master-g3/"
    "95e0d9a0-e724-4369-807f-4acdfe15873f/scratchpad")

CFG_ADDR = 0xFEC00000        # Grackle/MPC106 config address port (LE)
CFG_DATA = 0xFEE00000        # config data port (LE)
BAR0 = 0x84000000            # VRAM aperture
BAR2 = 0x82000000            # 16KB register MMIO
FIFO = BAR2 + 0x1000         # PM4 PIO FIFO window (any dword 0x1000..0x13ff)
VRAM_SIZE = 32 * 1024 * 1024

# _C context registers (see ati_rage128_regs.h)
DST_PITCH_OFFSET_C = 0x1C80
DP_GUI_MASTER_CNTL_C = 0x1C84
SC_TOP_LEFT_C = 0x1C88
SC_BOTTOM_RIGHT_C = 0x1C8C
Z_OFFSET_C = 0x1C90
Z_PITCH_C = 0x1C94
Z_STEN_CNTL_C = 0x1C98
TEX_CNTL_C = 0x1C9C
PLANE_3D_MASK_C = 0x1D44
DST_PITCH_OFFSET = 0x142C
DP_CNTL = 0x16C0

GMC_ARGB1555 = 0x28CC33DB    # Nanosaur's GMC_C fixture: dst datatype 3
Z_TEST_LESS = 0x10
TEX_Z_ON = 0x3               # Z_ENABLE | Z_WRITE_ENABLE
TEX_Z_OFF = 0x0

results = []


def report(name, ok, detail):
    tag = "PASS" if ok else "FAIL"
    results.append((name, ok))
    print(f"{tag}: {name}: {detail}")


class QTest:
    def __init__(self, sock_path):
        self.proc = subprocess.Popen(
            [QEMU, "-machine", "g3beige,accel=qtest", "-display", "none",
             "-qtest", f"unix:{sock_path},server", "-qtest-log", "/dev/null",
             "-device", "ati-rage128-pro"],
            cwd=SCRATCH,   # g3beige drops an nvram.img into its cwd
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.sock.connect(sock_path)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if self.proc.poll() is not None:
                    sys.exit("QEMU exited early:\n" +
                             self.proc.stderr.read().decode())
                time.sleep(0.1)
        else:
            sys.exit("could not connect to qtest socket")
        self.buf = b""

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        while True:
            while b"\n" not in self.buf:
                chunk = self.sock.recv(65536)
                if not chunk:
                    sys.exit(f"qtest EOF after: {line[:80]}")
                self.buf += chunk
            resp, self.buf = self.buf.split(b"\n", 1)
            resp = resp.decode()
            if resp.startswith("IRQ"):
                continue
            if not resp.startswith("OK"):
                sys.exit(f"qtest error '{resp}' for: {line[:80]}")
            return resp

    # 32-bit access to little-endian regions (PCI config ports, BAR2
    # MMIO): qtest writel/readl move target-endian (BE on ppc) byte
    # images, so the value must be pre/post byte-swapped.
    def wl(self, addr, val):
        self.cmd("writel 0x%x 0x%x" % (addr, struct.unpack("<I",
                 struct.pack(">I", val & 0xFFFFFFFF))[0]))

    def rl(self, addr):
        v = int(self.cmd("readl 0x%x" % addr).split()[1], 16)
        return struct.unpack("<I", struct.pack(">I", v))[0]

    def memset(self, addr, size, pattern):
        self.cmd("memset 0x%x 0x%x 0x%x" % (addr, size, pattern))

    def b64read(self, addr, size):
        return base64.b64decode(self.cmd("b64read 0x%x 0x%x"
                                         % (addr, size)).split()[1])

    def close(self):
        try:
            self.sock.close()
        finally:
            self.proc.terminate()
            self.proc.wait()


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


class Card:
    def __init__(self, q):
        self.q = q
        self.devfn = None
        for devfn in range(0, 256, 8):
            q.wl(CFG_ADDR, 0x80000000 | devfn << 8)
            if q.rl(CFG_DATA) == 0x52451002:      # 0x1002:0x5245
                self.devfn = devfn
                break
        if self.devfn is None:
            sys.exit("ati-rage128-pro not found on PCI bus")
        self.cfg_write(0x10, BAR0)
        self.cfg_write(0x18, BAR2)
        self.cfg_write(0x04, self.cfg_read(0x04) | 0x2)   # memory enable

    def cfg_read(self, reg):
        self.q.wl(CFG_ADDR, 0x80000000 | self.devfn << 8 | (reg & 0xFC))
        return self.q.rl(CFG_DATA)

    def cfg_write(self, reg, val):
        self.q.wl(CFG_ADDR, 0x80000000 | self.devfn << 8 | (reg & 0xFC))
        self.q.wl(CFG_DATA, val)

    def push(self, *dwords):
        for d in dwords:
            self.q.wl(FIFO, d)

    def reg(self, offset, val):
        """packet0 write of one register."""
        self.push(offset >> 2, val)

    def packet3(self, opcode, payload):
        self.push(0xC0000000 | (len(payload) - 1) << 16 | opcode << 8,
                  *payload)

    def state(self, dst_offset, pitch=0x50, gmc=GMC_ARGB1555,
              sc=(0, 0, 639, 479), z_offset=0x100000, z_pitch=0x50,
              z_sten=Z_TEST_LESS, tex_cntl=TEX_Z_OFF, plane_mask=0xFFFFFFFF):
        self.reg(DST_PITCH_OFFSET_C, pitch << 21 | dst_offset >> 5)
        self.reg(DP_GUI_MASTER_CNTL_C, gmc)
        left, top, right, bottom = sc
        self.reg(SC_TOP_LEFT_C, top << 16 | left)
        self.reg(SC_BOTTOM_RIGHT_C, bottom << 16 | right)
        self.reg(Z_OFFSET_C, z_offset)
        self.reg(Z_PITCH_C, z_pitch)
        self.reg(Z_STEN_CNTL_C, z_sten)
        self.reg(TEX_CNTL_C, tex_cntl)
        self.reg(PLANE_3D_MASK_C, plane_mask)

    def tri(self, verts, z=0.5):
        """One TRI_LIST GEN_PRIM; verts = [(x, y, (r,g,b,a)), ...] or
        (x, y, z, (r,g,b,a))."""
        payload = [0x07,                                   # RHW|BGR|A: 8 dw
                   len(verts) << 16 | 0x30 | 4]            # TRI_LIST
        for v in verts:
            if len(v) == 4:
                x, y, vz, (r, g, b, a) = v
            else:
                x, y, (r, g, b, a) = v
                vz = z
            payload += [f32(x), f32(y), f32(vz), f32(1.0),
                        f32(b), f32(g), f32(r), f32(a)]
        self.packet3(0x25, payload)


def pix16(fb, pitch_px, x, y):
    o = (y * pitch_px + x) * 2
    return struct.unpack_from("<H", fb, o)[0]


def count16(fb, value=None):
    n = 0
    for (p,) in struct.iter_unpack("<H", fb):
        if (value is None and p != 0) or (value is not None and p == value):
            n += 1
    return n


RED = (1.0, 0.0, 0.0, 1.0)
GREEN = (0.0, 1.0, 0.0, 1.0)
BLUE = (0.0, 0.0, 1.0, 1.0)


def check1(q, c):
    off, rows = 0x8000, 200
    q.memset(BAR0 + off, rows * 1280, 0)
    c.state(off, tex_cntl=TEX_Z_OFF)
    c.tri([(10.0, 10.0, RED), (200.0, 10.0, RED), (10.0, 150.0, RED)])
    fb = q.b64read(BAR0 + off, rows * 1280)
    inner = pix16(fb, 640, 50, 50)
    outer = pix16(fb, 640, 250, 100)
    n = count16(fb, 0xFC00)
    area = 0.5 * 190 * 140
    ok = inner == 0xFC00 and outer == 0 and abs(n - area) <= area * 0.01
    report("known-answer triangle", ok,
           f"inner(50,50)=0x{inner:04x} (want 0xfc00), "
           f"outer(250,100)=0x{outer:04x} (want 0x0000), "
           f"painted={n} vs analytic {area:.0f} "
           f"(delta {n - area:+.0f}, tol ±{area * 0.01:.0f})")


def check2(q, c):
    off, rows = 0x200000, 260
    verts = [(50.0, 50.0), (250.0, 60.0), (60.0, 250.0)]
    cols = [RED, GREEN, BLUE]
    q.memset(BAR0 + off, rows * 2560, 0)
    gmc8888 = (GMC_ARGB1555 & ~0xF00) | 6 << 8
    c.state(off, gmc=gmc8888, tex_cntl=TEX_Z_OFF)
    c.tri([(x, y, col) for (x, y), col in zip(verts, cols)])
    fb = q.b64read(BAR0 + off, rows * 2560)

    def bary(px, py):
        (x0, y0), (x1, y1), (x2, y2) = verts
        sx, sy = px + 0.5, py + 0.5
        area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)
        w0 = ((x2 - x1) * (sy - y1) - (y2 - y1) * (sx - x1)) / area
        w1 = ((x0 - x2) * (sy - y2) - (y0 - y2) * (sx - x2)) / area
        return w0, w1, 1.0 - w0 - w1

    cx = sum(x for x, y in verts) / 3.0
    cy = sum(y for x, y in verts) / 3.0
    ok, details = True, []
    for i, (vx, vy) in enumerate(verts):
        # a pixel a few percent of the way toward the centroid: barely
        # inside, so its interpolated colour is nearly the vertex colour
        px, py = int(vx + 0.06 * (cx - vx)), int(vy + 0.06 * (cy - vy))
        w = bary(px, py)
        want = tuple(round(255 * min(1.0, max(0.0,
                     sum(w[k] * cols[k][ch] for k in range(3)))))
                     for ch in range(3))
        got = struct.unpack_from("<I", fb, (py * 640 + px) * 4)[0]
        gr, gg, gb = got >> 16 & 0xFF, got >> 8 & 0xFF, got & 0xFF
        vok = all(abs(a - b) <= 8 for a, b in zip((gr, gg, gb), want))
        ok = ok and vok
        details.append(f"v{i}@({px},{py}) rgb=({gr},{gg},{gb}) "
                       f"want~{want}")
    report("gouraud interpolation (ARGB8888)", ok, "; ".join(details))


def check3(q, c):
    off, zoff, rows = 0x8000, 0x100000, 260
    far = [(50.0, 50.0, 0.8, RED), (250.0, 50.0, 0.8, RED),
           (50.0, 250.0, 0.8, RED)]
    near = [(50.0, 50.0, 0.3, GREEN), (250.0, 50.0, 0.3, GREEN),
            (50.0, 250.0, 0.3, GREEN)]

    def reset():
        q.memset(BAR0 + off, rows * 1280, 0)
        q.memset(BAR0 + zoff, rows * 1280, 0xFF)

    def probe():
        return struct.unpack("<H", q.b64read(
            BAR0 + off + (80 * 640 + 80) * 2, 2))[0]

    c.state(off, z_offset=zoff, z_sten=Z_TEST_LESS, tex_cntl=TEX_Z_ON)
    reset()
    c.tri(far)
    c.tri(near)
    a = probe()
    reset()
    c.tri(near)
    c.tri(far)
    b = probe()
    c.state(off, z_offset=zoff, z_sten=Z_TEST_LESS, tex_cntl=TEX_Z_OFF)
    reset()
    c.tri(near)
    c.tri(far)
    ctl = probe()
    ok = a == 0x83E0 and b == 0x83E0 and ctl == 0xFC00
    report("z test LESS", ok,
           f"far-then-near=0x{a:04x} (want 0x83e0 green), "
           f"near-then-far=0x{b:04x} (want 0x83e0 green), "
           f"z-off control=0x{ctl:04x} (want 0xfc00 red, last drawn)")


def check4(q, c):
    off, rows = 0x8000, 100
    tri = [(20.0, 20.0, RED), (80.0, 20.0, RED), (20.0, 80.0, RED)]
    q.memset(BAR0 + off, rows * 1280, 0)
    c.state(off, sc=(150, 150, 200, 200), tex_cntl=TEX_Z_OFF)
    c.tri(tri)
    closed = count16(q.b64read(BAR0 + off, rows * 1280))
    c.state(off, sc=(0, 0, 639, 479), tex_cntl=TEX_Z_OFF)
    c.tri(tri)
    open_ = count16(q.b64read(BAR0 + off, rows * 1280))
    ok = closed == 0 and open_ > 0
    report("scissor negative control", ok,
           f"excluding scissor painted {closed} px (want 0); "
           f"open scissor painted {open_} px (positive control, want >0)")


def check5(q, c):
    dst = 0x0F0300 << 5                       # corpus DST_PITCH_OFFSET_C
    zoff = 0x1C00000
    pkts, cur = [], None
    with gzip.open(CORPUS, "rt") as f:
        for line in f:
            m = re.search(r"payload\[(\d+)\]=0x([0-9a-f]+)", line)
            if not m:
                continue
            if int(m.group(1)) == 0:
                cur = []
                pkts.append(cur)
            if cur is not None:
                cur.append(int(m.group(2), 16))
    pkts = [p for p in pkts if len(p) == 35]
    q.memset(BAR0 + dst, 480 * 1280, 0)
    q.memset(BAR0 + zoff, 480 * 1280, 0xFF)
    c.reg(DST_PITCH_OFFSET_C, 0x0A0F0300)     # corpus values verbatim
    c.reg(DP_GUI_MASTER_CNTL_C, 0x28CC33DB)
    c.reg(SC_TOP_LEFT_C, 0x00000000)
    c.reg(SC_BOTTOM_RIGHT_C, 0x01DF027F)
    c.reg(Z_OFFSET_C, zoff)
    c.reg(Z_PITCH_C, 0x00010050)
    c.reg(Z_STEN_CNTL_C, 0x00000010)
    c.reg(TEX_CNTL_C, 0x00000193)
    c.reg(PLANE_3D_MASK_C, 0xFFFFFFFF)
    for p in pkts:
        c.packet3(0x25, p)
    painted = 0
    for row0 in range(0, 480, 64):
        n = min(64, 480 - row0)
        painted += count16(q.b64read(BAR0 + dst + row0 * 1280, n * 1280))
    alive = c.cfg_read(0x00) == 0x52451002    # still answering = no crash
    ok = painted > 0 and alive
    report("corpus replay", ok,
           f"{len(pkts)} GEN_PRIM packets replayed, {painted} px non-zero "
           f"in the 640x480 target (want >0), qemu alive={alive}")


def check6(q, c):
    off, rows = 0x8000, 70                    # rows 290..359 of the target
    base = off + 290 * 1280
    q.memset(BAR0 + base, rows * 1280, 0)
    c.reg(DST_PITCH_OFFSET, 0x50 << 21 | off >> 5)
    c.reg(DP_CNTL, 0x3)
    # 4-dword PAINT (opcode 0x91): GMC (solid brush, PATCOPY, dt 3,
    # DST_PITCH_OFFSET_CNTL), colour, top-left, bottom-right
    c.packet3(0x91, [0x00F003D2, 0x7C1F7C1F,
                     300 << 16 | 300, 350 << 16 | 400])
    fb = q.b64read(BAR0 + base, rows * 1280)
    inner = pix16(fb, 640, 310, 310 - 290)
    n = count16(fb, 0x7C1F)
    ok = inner == 0x7C1F and n == 5000
    report("2d paint unharmed", ok,
           f"inner(310,310)=0x{inner:04x} (want 0x7c1f), "
           f"filled={n} px (want 5000 = 100x50)")


def main():
    os.makedirs(SCRATCH, exist_ok=True)
    # the session scratchpad path is longer than AF_UNIX allows (104
    # bytes on macOS), so the socket alone lives in a short tempdir
    sock_dir = tempfile.mkdtemp(prefix="r128qt-")
    sock_path = os.path.join(sock_dir, "qtest.sock")
    q = QTest(sock_path)
    try:
        c = Card(q)
        print(f"ati-rage128-pro at devfn 0x{c.devfn:02x}, "
              f"BAR0=0x{BAR0:08x} BAR2=0x{BAR2:08x}")
        check1(q, c)
        check2(q, c)
        check3(q, c)
        check4(q, c)
        check5(q, c)
        check6(q, c)
    finally:
        q.close()
        if os.path.exists(sock_path):
            os.unlink(sock_path)
        os.rmdir(sock_dir)
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed"
          + (f"; FAILED: {', '.join(failed)}" if failed else ""))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
