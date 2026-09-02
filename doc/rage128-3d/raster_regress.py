#!/usr/bin/env python3
"""qtest regression harness for the Rage 128 3D software rasterizer.

Boots g3beige under -accel qtest (no guest code runs), finds the
ati-rage128-pro on the Grackle host bridge, programs BARs, and drives
the CCE PIO FIFO (BAR2 + 0x1000) with packet0 state writes and
GEN_PRIM packet3s -- the same path Nanosaur's RAVE driver uses.
Pixels are read back raw through the BAR0 VRAM aperture.

Checks 1-6 (HANDOFF-rasterizer.md): known-answer triangle, Gouraud
interpolation, Z test (+ negative control), scissor negative control,
corpus replay, and 2D-unharmed. Checks 7-16 (HANDOFF-textures-perf.md):
nearest texel exactness + orientation, bilinear, untextured control,
perspective correction, texture formats, alpha test, alpha blend, and
2D blit / host-data / scaler coverage. After every check the whole
target surface is CRC'd and compared against the GOLDEN table below
(recorded from the build that first passed each check), so a
rendering-path rewrite is proven byte-identical rather than assumed.

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
import zlib

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
QEMU = os.environ.get("RASTER_QEMU",
                      os.path.join(TREE, "build-g3", "qemu-system-ppc"))
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
MISC_3D_STATE_CNTL_REG = 0x1CA0
PRIM_TEX_CNTL_C = 0x1CB0
PRIM_TEXTURE_COMBINE_CNTL_C = 0x1CB4
TEX_SIZE_PITCH_C = 0x1CB8
PRIM_TEX_0_OFFSET_C = 0x1CBC          # slot n at 0x1CBC + 4n, n = 0..10
PRIM_TEXTURE_BORDER_COLOR_C = 0x1D38
PLANE_3D_MASK_C = 0x1D44
SRC_PITCH_OFFSET = 0x1428
DST_PITCH_OFFSET = 0x142C
BRUSH_Y_X = 0x1474
BRUSH_DATA0 = 0x1480
DP_CNTL = 0x16C0
SC_TOP_LEFT = 0x16EC
SC_BOTTOM_RIGHT = 0x16F0

GMC_ARGB1555 = 0x28CC33DB    # Nanosaur's GMC_C fixture: dst datatype 3
Z_TEST_LESS = 0x10
TEX_Z_ON = 0x3               # Z_ENABLE | Z_WRITE_ENABLE
TEX_Z_OFF = 0x0
TEXMAP_ENABLE = 1 << 4
ALPHA_ENABLE = 1 << 9        # blend
ALPHA_TEST_ENABLE = 1 << 10
# PRIM_TEX_CNTL_C values Nanosaur programs (datatype in bits 19:16)
TEXC_NEAREST = 0x01030080    # ARGB1555, nearest, wrap, mip off
TEXC_LINEAR = 0x010300B6     # ARGB1555, bilinear MAG, wrap, mip off
TEXC_LINEAR_CLAMP = 0x010312B6
COMB_MODULATE = 0x0418D043   # Nanosaur's combine on every textured tri
COMB_COPY = 0x0418D041       # texel replaces the colour (alpha modulate)

# Whole-target CRC32 after each check, recorded from the first build
# that passed it (Goal A, before the per-pixel overhead rewrite). An
# empty entry is reported as "recorded" and does not fail the run.
GOLDEN = {
    "known-answer triangle": 0xcb59e33d,
    "gouraud interpolation (ARGB8888)": 0x1ae441b9,
    "z test LESS": 0x3ae9c2d5,
    "scissor negative control": 0x14e0f3be,
    "corpus replay (textured)": 0x1835ab04,
    "2d paint unharmed": 0x1a3f841d,
    "texture nearest": 0x2056fb38,
    "texture bilinear": 0x9ec2611c,
    "untextured control": 0xcde45178,
    "perspective": 0xbe72dc11,
    "texture formats": 0x52a4f6f6,
    "alpha test": 0x3630126e,
    "alpha blend": 0xbc5220d7,
    "2d bitblt": 0x416ba8e2,
    "2d hostdata": 0xffbb35dc,
    "2d scaler": 0x5ef17b9f,
}

results = []


def report(name, ok, detail):
    tag = "PASS" if ok else "FAIL"
    results.append((name, ok))
    print(f"{tag}: {name}: {detail}")


def crc_check(name, data):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    want = GOLDEN.get(name)
    if want is None:
        print(f"      crc: {name}: 0x{crc:08x} (recorded, no golden)")
        return
    report(f"crc {name}", crc == want,
           f"0x{crc:08x} (golden 0x{want:08x}, {len(data)} bytes)")


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

    def b64write(self, addr, data):
        self.cmd("b64write 0x%x 0x%x %s"
                 % (addr, len(data), base64.b64encode(data).decode()))

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
              z_sten=Z_TEST_LESS, tex_cntl=TEX_Z_OFF, plane_mask=0xFFFFFFFF,
              misc=0):
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
        self.reg(MISC_3D_STATE_CNTL_REG, misc)

    def texture(self, base, log2w, log2h, cntl=TEXC_NEAREST,
                comb=COMB_MODULATE, border=0):
        """Program the primary texture unit: base offset into the slot
        TEX_SIZE selects (log2 width), pitch = width."""
        self.reg(PRIM_TEX_CNTL_C, cntl)
        self.reg(PRIM_TEXTURE_COMBINE_CNTL_C, comb)
        self.reg(TEX_SIZE_PITCH_C, log2h << 8 | log2w << 4 | log2w)
        self.reg(PRIM_TEX_0_OFFSET_C + 4 * log2w, base)
        self.reg(PRIM_TEXTURE_BORDER_COLOR_C, border)

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

    def ttri(self, verts, z=0.5):
        """One textured TRI_LIST GEN_PRIM in Nanosaur's vc_format 0xa7
        (x,y,z,rhw,b,g,r,a,fog,s,t); verts = [(x, y, rhw, (r,g,b,a),
        (s,t)), ...]."""
        payload = [0xA7, len(verts) << 16 | 0x30 | 4]
        for x, y, rhw, (r, g, b, a), (s, t) in verts:
            payload += [f32(x), f32(y), f32(z), f32(rhw),
                        f32(b), f32(g), f32(r), f32(a), f32(0.0),
                        f32(s), f32(t)]
        self.packet3(0x25, payload)

    def tquad(self, x0, y0, x1, y1, col=(1.0, 1.0, 1.0, 1.0),
              st0=(0.0, 0.0), st1=(1.0, 1.0), rhw_l=1.0, rhw_r=1.0):
        """Axis-aligned textured quad as two triangles sharing the
        diagonal; s/t map linearly over the rectangle, 1/w may differ
        between the left and right edges (perspective test)."""
        tl = (x0, y0, rhw_l, col, (st0[0], st0[1]))
        tr = (x1, y0, rhw_r, col, (st1[0], st0[1]))
        bl = (x0, y1, rhw_l, col, (st0[0], st1[1]))
        br = (x1, y1, rhw_r, col, (st1[0], st1[1]))
        self.ttri([tl, tr, bl])
        self.ttri([tr, br, bl])


def pix16(fb, pitch_px, x, y):
    o = (y * pitch_px + x) * 2
    return struct.unpack_from("<H", fb, o)[0]


def pix32(fb, pitch_px, x, y):
    return struct.unpack_from("<I", fb, (y * pitch_px + x) * 4)[0]


def count16(fb, value=None):
    n = 0
    for (p,) in struct.iter_unpack("<H", fb):
        if (value is None and p != 0) or (value is not None and p == value):
            n += 1
    return n


RED = (1.0, 0.0, 0.0, 1.0)
GREEN = (0.0, 1.0, 0.0, 1.0)
BLUE = (0.0, 0.0, 1.0, 1.0)
WHITE = (1.0, 1.0, 1.0, 1.0)


def x5(v):
    """5-bit channel to 8 bits, as the device expands texels."""
    return (v & 0x1f) << 3 | (v & 0x1f) >> 2


def x6(v):
    return (v & 0x3f) << 2 | (v & 0x3f) >> 4


# The 8x8 test texture is deliberately asymmetric: texel (x,y) encodes
# its own x in red (r5 = 4x+1), y in green (g5 = 4y+2) and a checker in
# blue/alpha, so a flipped or transposed fetch decodes to the wrong
# (x,y) instead of an identical-looking cell.
def tex_texel(x, y):
    """(a1, r5, g5, b5) of texel (x, y)."""
    return ((x + y) & 1, 4 * x + 1, 4 * y + 2, ((x + y) & 1) * 31)


def tex1555():
    data = b""
    for y in range(8):
        for x in range(8):
            a, r, g, b = tex_texel(x, y)
            data += struct.pack("<H", a << 15 | r << 10 | g << 5 | b)
    return data


def tex_argb(x, y):
    """The ARGB8888 value the device fetches for texel (x,y) of the
    ARGB1555 texture (5-bit channels expanded)."""
    a, r, g, b = tex_texel(x, y)
    return (0xFF if a else 0) << 24 | x5(r) << 16 | x5(g) << 8 | x5(b)


def argb_to_1555(argb):
    a, r, g, b = argb >> 24, argb >> 16 & 0xFF, argb >> 8 & 0xFF, argb & 0xFF
    return (0x8000 if a >= 128 else 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3


def decode_xy_1555(p):
    """texel (x, y) a 1555 output pixel came from, per tex_texel."""
    return ((p >> 10 & 0x1f) - 1) // 4, ((p >> 5 & 0x1f) - 2) // 4


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
    crc_check("known-answer triangle", fb)


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
    crc_check("gouraud interpolation (ARGB8888)", fb)


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
    crc_check("z test LESS", q.b64read(BAR0 + off, rows * 1280) +
              q.b64read(BAR0 + zoff, rows * 1280))


def check4(q, c):
    off, rows = 0x8000, 100
    tri = [(20.0, 20.0, RED), (80.0, 20.0, RED), (20.0, 80.0, RED)]
    q.memset(BAR0 + off, rows * 1280, 0)
    c.state(off, sc=(150, 150, 200, 200), tex_cntl=TEX_Z_OFF)
    c.tri(tri)
    closed = count16(q.b64read(BAR0 + off, rows * 1280))
    c.state(off, sc=(0, 0, 639, 479), tex_cntl=TEX_Z_OFF)
    c.tri(tri)
    fb = q.b64read(BAR0 + off, rows * 1280)
    open_ = count16(fb)
    ok = closed == 0 and open_ > 0
    report("scissor negative control", ok,
           f"excluding scissor painted {closed} px (want 0); "
           f"open scissor painted {open_} px (positive control, want >0)")
    crc_check("scissor negative control", fb)


def check5(q, c):
    dst = 0x0F0300 << 5                       # corpus DST_PITCH_OFFSET_C
    zoff = 0x1C00000
    texbase = 0x01D0AE00                      # corpus PRIM_TEX_8_OFFSET_C
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
    # a 256x256 ARGB1555 texture where texel (x,y) = x*y-ish gradient,
    # never white, so a textured fetch is distinguishable from the
    # untextured fallback (which would paint the vertex colour)
    tex = bytearray()
    for y in range(256):
        for x in range(256):
            tex += struct.pack("<H", 0x8000 | (x >> 3) << 10 | (y >> 3) << 5
                               | ((x ^ y) >> 3))
    q.b64write(BAR0 + texbase, bytes(tex))
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
    c.reg(MISC_3D_STATE_CNTL_REG, 0x00510200)
    c.reg(PRIM_TEX_CNTL_C, 0x010300B6)
    c.reg(PRIM_TEXTURE_COMBINE_CNTL_C, 0x0418D043)
    c.reg(TEX_SIZE_PITCH_C, 0x03330888)
    for n, v in enumerate([0x3f800000, 0x40000000, 0x437f0000, 0x01ff3e00,
                           0x47000000, 0x4e7c0000, 0xc2400000, 0x40c00000,
                           0xc1d0ae00, 0x3fa20481, 0x20481205]):
        c.reg(PRIM_TEX_0_OFFSET_C + 4 * n, v)   # slot 8 = the real base
    c.reg(PLANE_3D_MASK_C, 0xFFFFFFFF)
    for p in pkts:
        c.packet3(0x25, p)
    fb = b""
    for row0 in range(0, 480, 64):
        n = min(64, 480 - row0)
        fb += q.b64read(BAR0 + dst + row0 * 1280, n * 1280)
    painted = count16(fb)
    distinct = len(set(p for (p,) in struct.iter_unpack("<H", fb) if p))
    alive = c.cfg_read(0x00) == 0x52451002    # still answering = no crash
    ok = painted > 0 and distinct > 8 and alive
    report("corpus replay (textured)", ok,
           f"{len(pkts)} GEN_PRIM packets replayed with the corpus texture "
           f"state, {painted} px non-zero in the 640x480 target (want >0), "
           f"{distinct} distinct colours (want >8: texels, not one flat "
           f"colour), qemu alive={alive}")
    crc_check("corpus replay (textured)", fb)


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
    crc_check("2d paint unharmed", fb)


def check7(q, c):
    """Nearest texel exactness and orientation, ARGB1555 texture on an
    ARGB1555 target: a 128x128 quad over the 8x8 texture puts each
    texel on a 16x16 block; every block centre must read back exactly
    its texel, decoded back to the (x,y) it encodes."""
    off, rows, texbase = 0x8000, 240, 0x300000
    q.memset(BAR0 + off, rows * 1280, 0)
    q.b64write(BAR0 + texbase, tex1555())
    c.state(off, tex_cntl=TEXMAP_ENABLE)
    c.texture(texbase, 3, 3, cntl=TEXC_NEAREST, comb=COMB_MODULATE)
    c.tquad(100.0, 100.0, 228.0, 228.0, col=WHITE)
    fb = q.b64read(BAR0 + off, rows * 1280)
    bad, checker = [], set()
    for ty in range(8):
        for tx in range(8):
            got = pix16(fb, 640, 100 + 16 * tx + 8, 100 + 16 * ty + 8)
            want = argb_to_1555(tex_argb(tx, ty))
            checker.add(got & 0x801F)
            if got != want:
                bad.append(f"({tx},{ty}) got 0x{got:04x} want 0x{want:04x} "
                           f"= texel {decode_xy_1555(got)}")
    painted = count16(fb)
    corner = pix16(fb, 640, 100, 100)
    ok = not bad and painted == 128 * 128 and len(checker) == 2 and \
        decode_xy_1555(corner) == (0, 0)
    report("texture nearest (ARGB1555, exact texels, orientation)", ok,
           f"64/64 block centres exact={not bad}, painted={painted} "
           f"(want 16384), checker colours seen={sorted(checker)} "
           f"(want 2), top-left pixel decodes to texel "
           f"{decode_xy_1555(corner)} (want (0, 0): t=0 is the first "
           f"row in memory, s=0 the first texel)"
           + ("; BAD: " + "; ".join(bad[:4]) if bad else ""))
    crc_check("texture nearest", fb)


def check8(q, c):
    """Bilinear, ARGB8888 target: quad shifted half a pixel so pixel
    centres land on texel centres (exact texel) and on texel midpoints
    (the mean of the neighbours, +-1 since a midpoint mean of two
    8-bit values is x.5 and the interpolated weight may be one ulp
    under 0.5); the last column's right neighbour wraps to texel 0
    with WRAP and stays 7 with CLAMP."""
    off, rows, texbase = 0x200000, 240, 0x300000
    gmc8888 = (GMC_ARGB1555 & ~0xF00) | 6 << 8
    q.b64write(BAR0 + texbase, tex1555())

    def draw(cntl):
        q.memset(BAR0 + off, rows * 2560, 0)
        c.state(off, gmc=gmc8888, tex_cntl=TEXMAP_ENABLE)
        c.texture(texbase, 3, 3, cntl=cntl, comb=COMB_MODULATE)
        # one texel past the edge in both axes so the last column/row's
        # midpoints (which wrap or clamp) are inside the quad
        c.tquad(99.5, 99.5, 243.5, 243.5, col=WHITE, st1=(1.125, 1.125))
        return q.b64read(BAR0 + off, rows * 2560)

    def mean(argbs):
        return [sum((v >> sh & 0xFF) for v in argbs) / len(argbs)
                for sh in (24, 16, 8, 0)]

    def near(got, want, tol):
        return all(abs((got >> sh & 0xFF) - w) <= tol
                   for sh, w in zip((24, 16, 8, 0), want))

    fb = draw(TEXC_LINEAR)
    bad, nearest_would_fail = [], 0
    for ty in range(8):
        for tx in range(8):
            cx, cy = 107 + 16 * tx, 107 + 16 * ty
            got = pix32(fb, 640, cx, cy)
            if got != tex_argb(tx, ty):
                bad.append(f"centre({tx},{ty}) 0x{got:08x}!="
                           f"0x{tex_argb(tx, ty):08x}")
            probes = [
                ("hmid", (cx + 8, cy), [(tx, ty), ((tx + 1) % 8, ty)]),
                ("vmid", (cx, cy + 8), [(tx, ty), (tx, (ty + 1) % 8)]),
                ("corner", (cx + 8, cy + 8),
                 [(tx, ty), ((tx + 1) % 8, ty), (tx, (ty + 1) % 8),
                  ((tx + 1) % 8, (ty + 1) % 8)])]
            for name, (px, py), texels in probes:
                got = pix32(fb, 640, px, py)
                want = mean([tex_argb(x, y) for x, y in texels])
                if not near(got, want, 1):
                    bad.append(f"{name}({tx},{ty}) 0x{got:08x} want ~"
                               + "/".join(f"{w:.1f}" for w in want))
                if not near(tex_argb(*texels[0]), want, 1):
                    nearest_would_fail += 1
    wrap_px = pix32(fb, 640, 107 + 16 * 7 + 8, 107)
    wrap_want = mean([tex_argb(7, 0), tex_argb(0, 0)])
    wrap_ok = near(wrap_px, wrap_want, 1)
    crc_a = fb
    fb = draw(TEXC_LINEAR_CLAMP)
    clamp_px = pix32(fb, 640, 107 + 16 * 7 + 8, 107)
    clamp_ok = clamp_px == tex_argb(7, 0)
    ok = not bad and wrap_ok and clamp_ok
    report("texture bilinear (centres exact, midpoints mean, wrap/clamp)",
           ok, f"64 centres exact + 192 midpoints within 1={not bad} (a "
           f"nearest fetch would miss {nearest_would_fail} of the "
           f"midpoints); wrap edge 0x{wrap_px:08x} ~ mean of texels 7,0 "
           f"={wrap_ok}; clamp edge 0x{clamp_px:08x} == texel 7 "
           f"0x{tex_argb(7, 0):08x} ={clamp_ok}"
           + ("; BAD: " + "; ".join(bad[:4]) if bad else ""))
    crc_check("texture bilinear", crc_a + fb)


def check9(q, c):
    """Untextured control: same texture registers, TEXMAP_ENABLE clear
    -> the Gouraud colour only."""
    off, rows, texbase = 0x8000, 240, 0x300000
    q.memset(BAR0 + off, rows * 1280, 0)
    q.b64write(BAR0 + texbase, tex1555())
    c.state(off, tex_cntl=TEX_Z_OFF)
    c.texture(texbase, 3, 3, cntl=TEXC_NEAREST, comb=COMB_MODULATE)
    c.tquad(100.0, 100.0, 228.0, 228.0, col=(1.0, 0.5, 0.0, 1.0))
    fb = q.b64read(BAR0 + off, rows * 1280)
    want = 0x8000 | 31 << 10 | (128 >> 3) << 5
    n = count16(fb, want)
    painted = count16(fb)
    ok = n == 16384 and painted == 16384
    report("untextured control (TEXMAP_ENABLE off)", ok,
           f"{n} px of the vertex colour 0x{want:04x} (want 16384), "
           f"{painted} painted")
    crc_check("untextured control", fb)


def check10(q, c):
    """Perspective correction: left edge 1/w = 1, right edge 1/w = 0.25.
    Texel column at pixel x must follow s = 0.25f / (1 - 0.75f),
    f = (x + 0.5 - 100) / 128, not the affine s = f."""
    off, rows, texbase = 0x8000, 240, 0x300000
    q.memset(BAR0 + off, rows * 1280, 0)
    q.b64write(BAR0 + texbase, tex1555())
    c.state(off, tex_cntl=TEXMAP_ENABLE)
    c.texture(texbase, 3, 3, cntl=TEXC_NEAREST, comb=COMB_MODULATE)
    c.tquad(100.0, 100.0, 228.0, 228.0, col=WHITE, st0=(0.0, 0.5),
            st1=(1.0, 0.5), rhw_l=1.0, rhw_r=0.25)
    fb = q.b64read(BAR0 + off, rows * 1280)
    bad, differ = [], 0
    for px in range(100, 228):
        f = (px + 0.5 - 100) / 128.0
        s = 0.25 * f / (1.0 - 0.75 * f)
        want = int(8 * s)
        affine = int(8 * f)
        got = decode_xy_1555(pix16(fb, 640, px, 164))
        if got != (want, 4):
            bad.append(f"x={px} got {got} want ({want}, 4)")
        if affine != want:
            differ += 1
    probe = decode_xy_1555(pix16(fb, 640, 180, 164))
    ok = not bad
    report("perspective-correct s/t via rhw", ok,
           f"128/128 columns match the 1/w-interpolated texel column="
           f"{not bad}; x=180 -> texel {probe} (perspective wants column 2, "
           f"affine would give 5); affine differs at {differ} columns"
           + ("; BAD: " + "; ".join(bad[:4]) if bad else ""))
    crc_check("perspective", fb)


def check11(q, c):
    """Texture formats RGB565, ARGB4444, ARGB8888 on an ARGB8888 target:
    exact expanded texels (alpha included) at every block centre."""
    off, rows = 0x200000, 240
    gmc8888 = (GMC_ARGB1555 & ~0xF00) | 6 << 8
    cases = []
    for dt, name in ((4, "RGB565"), (15, "ARGB4444"), (6, "ARGB8888")):
        data, want = b"", {}
        for y in range(8):
            for x in range(8):
                a, r, g, b = tex_texel(x, y)
                if dt == 4:
                    g6 = g << 1 | (x & 1)
                    data += struct.pack("<H", r << 11 | g6 << 5 | b)
                    want[x, y] = 0xFF000000 | x5(r) << 16 | x6(g6) << 8 | x5(b)
                elif dt == 15:
                    a4, r4, g4, b4 = (a * 15) ^ (x & 3), r & 15, g & 15, b & 15
                    data += struct.pack("<H", a4 << 12 | r4 << 8 | g4 << 4 | b4)
                    want[x, y] = (a4 * 17) << 24 | (r4 * 17) << 16 | \
                        (g4 * 17) << 8 | b4 * 17
                else:
                    v = (a * 200 + x) << 24 | (r * 8 + y) << 16 | \
                        (g * 8 + x) << 8 | (b * 8 + 3)
                    data += struct.pack("<I", v)
                    want[x, y] = v
        cases.append((dt, name, data, want))
    fbs = b""
    details, ok = [], True
    for dt, name, data, want in cases:
        texbase = 0x300000 + dt * 0x1000
        q.memset(BAR0 + off, rows * 2560, 0)
        q.b64write(BAR0 + texbase, data)
        c.state(off, gmc=gmc8888, tex_cntl=TEXMAP_ENABLE)
        c.texture(texbase, 3, 3, cntl=(TEXC_NEAREST & ~0xF0000) | dt << 16,
                  comb=COMB_MODULATE)
        c.tquad(100.0, 100.0, 228.0, 228.0, col=WHITE)
        fb = q.b64read(BAR0 + off, rows * 2560)
        fbs += fb
        bad = 0
        first = ""
        for ty in range(8):
            for tx in range(8):
                got = pix32(fb, 640, 100 + 16 * tx + 8, 100 + 16 * ty + 8)
                if got != want[tx, ty]:
                    bad += 1
                    first = first or f" e.g. ({tx},{ty}) 0x{got:08x} " \
                                     f"want 0x{want[tx, ty]:08x}"
        ok = ok and bad == 0
        details.append(f"{name}: {64 - bad}/64 exact{first}")
    report("texture formats (565/4444/8888 -> ARGB8888)", ok,
           "; ".join(details))
    crc_check("texture formats", fbs)


def check12(q, c):
    """Alpha test: texels with the 1555 alpha bit clear (half the
    checker) must be rejected under GREATER ref 0x80; positive
    control with the test disabled paints all of them."""
    off, rows, texbase = 0x8000, 240, 0x300000
    q.b64write(BAR0 + texbase, tex1555())
    misc = 5 << 24 | 0x80                     # ALPHA_TEST_GREATER, ref 128

    def draw(tex_cntl):
        q.memset(BAR0 + off, rows * 1280, 0)
        c.state(off, tex_cntl=tex_cntl, misc=misc)
        c.texture(texbase, 3, 3, cntl=TEXC_NEAREST, comb=COMB_MODULATE)
        c.tquad(100.0, 100.0, 228.0, 228.0, col=WHITE)
        return q.b64read(BAR0 + off, rows * 1280)

    fb = draw(TEXMAP_ENABLE | ALPHA_TEST_ENABLE)
    painted = count16(fb)
    wrong = 0
    for ty in range(8):
        for tx in range(8):
            got = pix16(fb, 640, 100 + 16 * tx + 8, 100 + 16 * ty + 8)
            if bool(got) != bool((tx + ty) & 1):
                wrong += 1
    ctl = count16(draw(TEXMAP_ENABLE))
    ok = painted == 8192 and wrong == 0 and ctl == 16384
    report("alpha test (GREATER 0x80 on 1555 texel alpha)", ok,
           f"painted={painted} (want 8192 = the opaque half), blocks "
           f"painted against their alpha bit={wrong} (want 0); test-off "
           f"control painted={ctl} (want 16384)")
    crc_check("alpha test", fb)


def check13(q, c):
    """Alpha blend SRCALPHA/INVSRCALPHA over a known ARGB8888 background:
    exact (sc*128 + dc*127 + 127) / 255 per channel."""
    off, rows = 0x200000, 240
    gmc8888 = (GMC_ARGB1555 & ~0xF00) | 6 << 8
    bg = 0xFF004080
    q.memset(BAR0 + off, rows * 2560, 0)
    q.b64write(BAR0 + off + 100 * 2560, struct.pack("<I", bg) * 640 * 100)
    misc = 4 << 16 | 5 << 20                  # src SRCALPHA, dst INVSRCALPHA
    c.state(off, gmc=gmc8888, tex_cntl=ALPHA_ENABLE, misc=misc)
    src = (1.0, 0.5, 0.0, 0.5)                # a8 = 128
    c.tri([(120.0, 120.0, src), (180.0, 120.0, src), (120.0, 180.0, src)])
    fb = q.b64read(BAR0 + off, rows * 2560)
    got = pix32(fb, 640, 130, 130)
    sc = (128, 255, 128, 0)                   # a, r, g, b
    dc = (bg >> 24, bg >> 16 & 0xFF, bg >> 8 & 0xFF, bg & 0xFF)
    want = 0
    for k in range(4):
        v = (sc[k] * 128 + dc[k] * 127 + 127) // 255
        want = want << 8 | min(v, 255)
    outside = pix32(fb, 640, 300, 130)
    ok = got == want and outside == bg
    report("alpha blend (SRCALPHA/INVSRCALPHA, ARGB8888)", ok,
           f"inner(130,130)=0x{got:08x} (want 0x{want:08x}), "
           f"outside=0x{outside:08x} (want 0x{bg:08x} untouched)")
    crc_check("alpha blend", fb)


def check14(q, c):
    """2D BITBLT coverage for the row-loop rewrite: a plain copy, a
    self-overlapping down-right copy (reverse walk), a PAINT through a
    transparent 8x8 mono brush, and a copy with a write mask."""
    off, rows = 0x8000, 200
    q.memset(BAR0 + off, rows * 1280, 0)
    # source block: gradient rows 0..49, x 0..99
    src = b"".join(struct.pack("<H", 0x8000 | (x & 31) << 10 | (y & 31) << 5
                               | ((x + y) & 31)) * 1
                   for y in range(50) for x in range(640))
    q.b64write(BAR0 + off, src)
    c.reg(SRC_PITCH_OFFSET, 0x50 << 21 | off >> 5)
    c.reg(DST_PITCH_OFFSET, 0x50 << 21 | off >> 5)
    c.reg(DP_CNTL, 0x3)
    gmc_copy = 0x02CC33D3        # memory src, SRCCOPY, dt 3, SRC+DST_PO
    # BITBLT: GMC, SRC_X_Y (x high), DST_X_Y, DST_WIDTH_HEIGHT (w high)
    c.packet3(0x92, [gmc_copy, 0 << 16 | 0, 200 << 16 | 60, 100 << 16 | 50])
    c.packet3(0x92, [gmc_copy, 200 << 16 | 60, 230 << 16 | 80,
                     100 << 16 | 50])                    # overlapping
    c.reg(BRUSH_DATA0, 0xAA55AA55)
    c.reg(BRUSH_DATA0 + 4, 0xAA55AA55)
    c.reg(BRUSH_Y_X, 0)
    c.packet3(0x91, [0x00F00312, 0x7C007C00, 120 << 16 | 400,
                     170 << 16 | 480])                   # 8x8 mono FG_LA
    c.reg(0x16CC, 0x7C00)                                # DP_WRITE_MASK
    c.packet3(0x92, [gmc_copy, 0 << 16 | 0, 500 << 16 | 120, 60 << 16 | 40])
    c.reg(0x16CC, 0xFFFFFFFF)
    fb = q.b64read(BAR0 + off, rows * 1280)
    # (210,65) is inside the first copy but outside the second's
    # destination; the overlapping copy must read its whole source
    # before it overwrites it, so (280,120) is the ORIGINAL (250,100)
    a = pix16(fb, 640, 210, 65) == pix16(fb, 640, 10, 5)
    b = pix16(fb, 640, 280, 120) == pix16(fb, 640, 50, 40)
    brush = sum(1 for y in range(120, 170) for x in range(400, 480)
                if pix16(fb, 640, x, y) == 0x7C00)
    masked = pix16(fb, 640, 510, 130)
    ok = a and b and brush == 2000 and masked == (pix16(fb, 640, 10, 10)
                                                  & 0x7C00)
    report("2d bitblt/brush/mask coverage", ok,
           f"copy exact={a}, overlapping copy exact={b}, mono brush "
           f"painted {brush} (want 2000 = half of 80x50), masked copy "
           f"0x{masked:04x} (want red bits of the source only)")
    crc_check("2d bitblt", fb)


def check15(q, c):
    """HOSTDATA_BLT coverage: 32x16 ARGB1555 pixels through the 8-dword
    Mac header form, must land exactly."""
    off, rows = 0x8000, 40
    q.memset(BAR0 + off, rows * 1280, 0)
    c.reg(DST_PITCH_OFFSET, 0x50 << 21 | off >> 5)
    pix = [((x * 7 + y * 13) & 0x7FFF) | 0x8000 for y in range(16)
           for x in range(32)]
    data = b"".join(struct.pack("<H", p) for p in pix)
    dwords = list(struct.unpack("<%dI" % (len(data) // 4), data))
    gmc = 0x03CC33D2             # host src, colour src dt, dt 3, DST_PO
    c.packet3(0x94, [gmc, 0, 0x3FFF3FFF, 0, 0, 5 << 16 | 20,
                     16 << 16 | 32, len(dwords)] + dwords)
    fb = q.b64read(BAR0 + off, rows * 1280)
    bad = sum(1 for y in range(16) for x in range(32)
              if pix16(fb, 640, 20 + x, 5 + y) != pix[y * 32 + x])
    n = count16(fb)
    ok = bad == 0 and n == 512
    report("2d hostdata blit coverage", ok,
           f"{512 - bad}/512 pixels exact, {n} painted (want 512)")
    crc_check("2d hostdata", fb)


def check16(q, c):
    """CNTL_SCALING coverage: a 16x16 ARGB1555 source scaled 2x."""
    off, rows, src = 0x8000, 60, 0x310000
    q.memset(BAR0 + off, rows * 1280, 0)
    data = b"".join(struct.pack(">H", 0x8000 | (x & 15) << 10 | (y & 15) << 5
                                | 1) for y in range(16) for x in range(16))
    q.b64write(BAR0 + src, data)
    pkt = [0] * 16
    pkt[0] = 0x02CC33D3
    pkt[1] = 0x2 << 21 | src >> 5
    pkt[2] = 0x50 << 21 | off >> 5
    pkt[3] = 0
    pkt[4] = 0x3FFF3FFF
    pkt[8] = 3
    pkt[9] = src
    pkt[10] = 2                  # pitch 16 px / 8
    pkt[12] = 0x800 << 4         # 0.5 in 4.12: 2x upscale
    pkt[13] = 0x800 << 4
    pkt[14] = 30 << 16 | 10
    pkt[15] = 32 << 16 | 32
    c.packet3(0x96, pkt)
    fb = q.b64read(BAR0 + off, rows * 1280)
    n = count16(fb)
    p = pix16(fb, 640, 30 + 2 * 5, 10 + 2 * 3)
    # the scaler expands the (big-endian) 1555 source to ARGB and packs
    # its 16-bit output as 565 regardless of the destination datatype
    # (ati_rage128_argb_to_dst) -- pre-existing engine behaviour, pinned
    # here so the row-loop rewrite cannot change it unnoticed
    want = 5 << 11 | (x5(3) >> 2) << 5 | x5(1) >> 3
    ok = n == 1024 and p == want
    report("2d scaler coverage", ok,
           f"{n} painted (want 1024 = 32x32), (5,3)x2 -> 0x{p:04x} "
           f"(want 0x{want:04x}, the engine's 565 packing)")
    crc_check("2d scaler", fb)


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
        for chk in (check1, check2, check3, check4, check5, check6, check7,
                    check8, check9, check10, check11, check12, check13,
                    check14, check15, check16):
            chk(q, c)
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
