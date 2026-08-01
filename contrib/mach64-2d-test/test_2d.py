import socket, sys, struct

class QTest:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rw')
    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        while True:
            r = self.f.readline().strip()
            if r.startswith("OK") or r.startswith("FAIL"):
                return r
    def writel_le(self, addr, val):   # write 32-bit little-endian raw bytes
        data = struct.pack('<I', val).hex()
        return self.cmd(f"write 0x{addr:x} 4 0x{data}")
    def readl_le(self, addr):
        r = self.cmd(f"read 0x{addr:x} 4")
        return struct.unpack('<I', bytes.fromhex(r.split()[1][2:]))[0]
    def write_bytes(self, addr, b):
        return self.cmd(f"write 0x{addr:x} {len(b)} 0x{b.hex()}")
    def read_bytes(self, addr, n):
        r = self.cmd(f"read 0x{addr:x} {n}")
        return bytes.fromhex(r.split()[1][2:])

q = QTest(sys.argv[1])

CFG_ADDR, CFG_DAT = 0xfec00000, 0xfee00000
def cfg_read(devfn, reg):
    # grackle CONFIG_ADDR is little-endian mmio
    q.writel_le(CFG_ADDR, 0x80000000 | (devfn << 8) | reg)
    return q.readl_le(CFG_DAT)
def cfg_write(devfn, reg, val):
    q.writel_le(CFG_ADDR, 0x80000000 | (devfn << 8) | reg)
    q.writel_le(CFG_DAT, val)

ati = None
for dev in range(32):
    v = cfg_read(dev << 3, 0)
    if (v & 0xffff) == 0x1002:
        ati = dev << 3
        print(f"ATI at devfn {ati}, id=0x{v:08x}")
        break
assert ati is not None, "ATI not found"

VRAM_BASE = 0x82000000
MMIO_BASE = 0x83000000
cfg_write(ati, 0x10, VRAM_BASE)   # BAR0 vram
cfg_write(ati, 0x18, MMIO_BASE)   # BAR2 mmio (check BAR layout)
# enable memory decode + bus mastering (the DMA tests need the latter)
cfg_write(ati, 4, cfg_read(ati, 4) | 6)
# check BARs landed
print("BAR0:", hex(cfg_read(ati, 0x10)), "BAR2:", hex(cfg_read(ati, 0x18)))

# BAR2 is not a flat view of the register file: its lower 1KB is
# register block 1 and its second 1KB is block 0 (the upper 2KB wraps
# onto those). Map a register's documented byte offset accordingly.
def regaddr(off):
    return MMIO_BASE + (off + 0x400 if off < 0x400 else off - 0x400)

R = MMIO_BASE
def reg(off, val):
    q.writel_le(regaddr(off), val)
def regread(off):
    return q.readl_le(regaddr(off))

PITCH = 640
SC_OPEN_LR = 0x1fff0000           # left=0 right=0x1fff
SC_OPEN_TB = 0x3fff0000           # top=0 bottom=0x3fff (SIGNED 15-bit field)
npass = nfail = 0
def result(name, ok, extra=""):
    global npass, nfail
    print(f"{name}:", "PASS" if ok else "FAIL", extra if not ok else "")
    npass += ok
    nfail += not ok

def engine_defaults(bpp_code=0x2, pitch=PITCH):
    reg(0x2d0, bpp_code)                # DP_PIX_WIDTH
    reg(0x2d4, 0x70003)                 # DP_MIX: frgd=S, bkgd=D
    reg(0x2c8, 0xffffffff)              # DP_WRITE_MSK
    reg(0x308, 0)                       # CLR_CMP_CNTL off
    reg(0x2a8, SC_OPEN_LR)
    reg(0x2b4, SC_OPEN_TB)
    reg(0x100, (pitch // 8) << 22)      # DST_OFF_PITCH offset 0
    reg(0x180, (pitch // 8) << 22)      # SRC_OFF_PITCH offset 0
    reg(0x130, 0x3)                     # DST_CNTL: L2R, T2B

# --- test 1: 8bpp solid fill 10x4 at (5,3) ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 16))
engine_defaults()
reg(0x2d8, 0x100)                   # DP_SRC: frgd = FRGD_CLR, mono=one
reg(0x2c4, 0xAB)                    # DP_FRGD_CLR
reg(0x10c, (5 << 16) | 3)           # DST_Y_X
reg(0x118, (10 << 16) | 4)          # DST_HEIGHT_WIDTH -> trigger
ok = True
for y in range(16):
    row = q.read_bytes(VRAM_BASE + y * PITCH, 20)
    for x in range(20):
        expect = 0xAB if (3 <= y < 7 and 5 <= x < 15) else 0x11
        if row[x] != expect:
            print(f"FILL MISMATCH at ({x},{y}): {row[x]:#x} != {expect:#x}")
            ok = False
result("fill", ok)

# --- test 2: overlapping copy downward (scroll down by 2 rows) ---
pat = bytes((y * 37 + x * 11) & 0xff for y in range(12) for x in range(PITCH))
q.write_bytes(VRAM_BASE, pat)
reg(0x2d8, 0x300)                   # DP_SRC: frgd = BLIT
reg(0x18c, (0 << 16) | 0)           # SRC_Y_X = (0,0)
reg(0x198, (PITCH << 16) | 10)      # SRC_HEIGHT1_WIDTH1
reg(0x10c, (0 << 16) | 2)           # DST_Y_X = (0,2)
reg(0x118, (PITCH << 16) | 10)      # DST -> trigger: copy 640x10 down 2
ok = True
after = q.read_bytes(VRAM_BASE, PITCH * 12)
for y in range(12):
    for x in range(0, PITCH, 53):
        expect = pat[y * PITCH + x] if y < 2 else pat[(y - 2) * PITCH + x]
        got = after[y * PITCH + x]
        if got != expect:
            print(f"COPY MISMATCH at ({x},{y}): {got:#x} != {expect:#x}")
            ok = False
result("overlap-copy", ok)

# --- test 3: scissored fill ---
q.write_bytes(VRAM_BASE, b'\x22' * (PITCH * 16))
reg(0x2a8, (12 << 16) | 8)          # SC_LEFT_RIGHT: left=8 right=12
reg(0x2b4, (9 << 16) | 6)           # SC_TOP_BOTTOM: top=6 bottom=9
reg(0x2d8, 0x100)
reg(0x2c4, 0xCD)
reg(0x10c, 0)
reg(0x118, (100 << 16) | 16)        # huge fill -> must clip to scissors
ok = True
for y in range(16):
    row = q.read_bytes(VRAM_BASE + y * PITCH, 20)
    for x in range(20):
        expect = 0xCD if (6 <= y <= 9 and 8 <= x <= 12) else 0x22
        if row[x] != expect:
            print(f"SCISSOR MISMATCH at ({x},{y}): {row[x]:#x} != {expect:#x}")
            ok = False
result("scissor", ok)

# --- test 4: 16bpp XOR fill ---
q.write_bytes(VRAM_BASE, b'\x0f\xf0' * (PITCH * 4))
engine_defaults(0x4)
reg(0x2d4, 0x50003)                 # frgd mix = XOR
reg(0x2d8, 0x100)
reg(0x2c4, 0xFFFF)
reg(0x10c, (2 << 16) | 1)
reg(0x118, (4 << 16) | 2)           # 4x2 pixels at (2,1)
ok = True
for y in range(4):
    row = q.read_bytes(VRAM_BASE + y * PITCH * 2, 16)
    for x in range(8):
        inside = (1 <= y <= 2 and 2 <= x <= 5)
        exp = struct.unpack('<H', bytes([0x0f ^ (0xff if inside else 0),
                                         0xf0 ^ (0xff if inside else 0)]))[0]
        got = struct.unpack('<H', row[x*2:x*2+2])[0]
        if got != exp:
            print(f"XOR MISMATCH at ({x},{y}): {got:#x} != {exp:#x}")
            ok = False
result("xor16", ok)

# --- test 5: right-to-left / bottom-to-top copy (corner-start coords) ---
pat = bytes((y * 7 + x * 3) & 0xff for y in range(12) for x in range(PITCH))
q.write_bytes(VRAM_BASE, pat)
engine_defaults()
reg(0x2d8, 0x300)                   # BLIT
reg(0x130, 0x0)                     # DST_CNTL: R2L, B2T -> coords are corner
# copy the 6x4 rect whose top-left is (10,2) to top-left (30,5):
# bottom-right src corner = (15,5), dst corner = (35,8)
reg(0x18c, (15 << 16) | 5)          # SRC_Y_X (corner)
reg(0x198, (6 << 16) | 4)
reg(0x10c, (35 << 16) | 8)          # DST_Y_X (corner)
reg(0x118, (6 << 16) | 4)           # trigger
ok = True
after = q.read_bytes(VRAM_BASE, PITCH * 12)
for y in range(12):
    for x in range(45):
        if 5 <= y <= 8 and 30 <= x <= 35:
            expect = pat[(y - 3) * PITCH + (x - 20)]
        else:
            expect = pat[y * PITCH + x]
        got = after[y * PITCH + x]
        if got != expect:
            print(f"DIR MISMATCH at ({x},{y}): {got:#x} != {expect:#x}")
            ok = False
result("direction", ok)

# --- test 6: mono host blit, documented order (bytes low->high, MSB
# first in each byte; HOST_BYTE_ALIGN re-aligns each row to a BYTE) ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 8))
engine_defaults()
reg(0x2c4, 0xEE)
reg(0x2c0, 0x22)
reg(0x2d4, 0x70007)                 # frgd=S, bkgd=S (opaque expansion)
reg(0x2d8, 0x20100)                 # MONO_SRC=HOST | FRGD_SRC=FRGD_CLR
reg(0x240, 0x1)                     # HOST_CNTL: BYTE_ALIGN
reg(0x10c, (4 << 16) | 3)           # DST_Y_X
reg(0x118, (8 << 16) | 2)           # 8x2 -> arm host blit
row0 = 0b10110001
row1 = 0b01001110
# one word carries both 8-pixel byte-aligned rows: row0=byte0, row1=byte1
reg(0x200, row0 | (row1 << 8))
ok = True
def expect_mono(bits, x):           # MSB of the row byte = leftmost pixel
    return 0xEE if (bits >> (7 - x)) & 1 else 0x22
for yy in range(8):
    r = q.read_bytes(VRAM_BASE + yy * PITCH, 16)
    for xx in range(16):
        if yy == 3 and 4 <= xx < 12:
            e = expect_mono(row0, xx - 4)
        elif yy == 4 and 4 <= xx < 12:
            e = expect_mono(row1, xx - 4)
        else:
            e = 0x11
        if r[xx] != e:
            print(f"MONO MISMATCH ({xx},{yy}): {r[xx]:#x} != {e:#x}")
            ok = False
result("mono-host", ok)

# --- test 6b: HOST_CNTL=0x2 (BIG_ENDIAN_EN) does NOT re-order 1bpp
# mask data (the translation is defined for 15/16/32bpp only), so the
# stream stays byte 0 first, MSB first within each byte ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 8))
reg(0x240, 0x2)                     # HOST_CNTL: BIG_ENDIAN_EN
reg(0x10c, (4 << 16) | 3)
reg(0x118, (12 << 16) | 2)          # 12x2, packed: 24 bits used
bits = 0
rowA, rowB = 0b101100011110, 0b010011100101   # 12px each
# packed MSB-first within each byte, byte 0 first
bits = (rowA << 12) | rowB                   # 24 bits, MSB first
stream = ((bits >> 16) & 0xff) | (((bits >> 8) & 0xff) << 8) | ((bits & 0xff) << 16)
reg(0x200, stream)
ok = True
for yy in range(8):
    r = q.read_bytes(VRAM_BASE + yy * PITCH, 20)
    for xx in range(20):
        if yy == 3 and 4 <= xx < 16:
            e = 0xEE if (rowA >> (11 - (xx - 4))) & 1 else 0x22
        elif yy == 4 and 4 <= xx < 16:
            e = 0xEE if (rowB >> (11 - (xx - 4))) & 1 else 0x22
        else:
            e = 0x11
        if r[xx] != e:
            print(f"MONO-BE MISMATCH ({xx},{yy}): {r[xx]:#x} != {e:#x}")
            ok = False
result("mono-host-bigendian", ok)

# --- test 6c: transparent mono expansion (bkgd mix = D leaves dst) ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 4))
reg(0x2d4, 0x70003)                 # frgd=S, bkgd=D
reg(0x240, 0x1)
reg(0x10c, (0 << 16) | 0)
reg(0x118, (8 << 16) | 1)
reg(0x200, 0b10100101)              # byte0, MSB first
r = q.read_bytes(VRAM_BASE, 8)
exp = [0xEE if (0b10100101 >> (7 - i)) & 1 else 0x11 for i in range(8)]
result("mono-host-transparent", list(r[:8]) == exp, f"got {r.hex()}")

# --- test 7: 8bpp packed-color host blit ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 8))
engine_defaults()
reg(0x2d0, 0x20002)                 # dst 8bpp | host 8bpp
reg(0x2d4, 0x70007)
reg(0x2d8, 0x200)                   # FRGD_SRC=HOST, MONO_SRC=ONE
reg(0x240, 0x1)
reg(0x10c, (2 << 16) | 1)           # (x=2,y=1)
reg(0x118, (4 << 16) | 1)           # 4x1
# one word = 4 packed 8bpp pixels, low byte first (8bpp gets no
# big-endian translation)
reg(0x200, 0xD3C2B1A0)
r = q.read_bytes(VRAM_BASE + 1 * PITCH, 8)
exp = [0x11,0x11,0xA0,0xB1,0xC2,0xD3,0x11,0x11]
result("color-host", all(r[i]==exp[i] for i in range(8)),
       f"got {[hex(b) for b in r[:8]]}")

# --- test 7b: 16bpp host color with BIG_ENDIAN_EN (bytes of each word
# swapped, pixels then consumed low-half first; the engine stores
# multi-byte pixels big-endian, matching this card's guests) ---
q.write_bytes(VRAM_BASE, b'\x00' * (PITCH * 4))
reg(0x2d0, 0x40004)                 # dst 16bpp | host 16bpp
reg(0x240, 0x2)                     # BIG_ENDIAN_EN
reg(0x10c, (1 << 16) | 0)
reg(0x118, (2 << 16) | 1)           # 2x1 at (1,0)
# BE translation swaps the bytes within each 16-bit half, then the
# low half is consumed first: p0 = 0x4433, p1 = 0x2211
reg(0x200, 0x11223344)
r = q.read_bytes(VRAM_BASE, 8)
got = struct.unpack('>4H', r)       # big-endian pixels in VRAM
result("color-host16-be", got[1] == 0x4433 and got[2] == 0x2211,
       f"got {[hex(v) for v in got]}")

# --- test 7c: 15bpp solid fill lands big-endian in VRAM (the NDRV
# programs the natural color value, e.g. 0x4210 = mid gray, and the
# guest's framebuffer is big-endian -- byte order verified live) ---
q.write_bytes(VRAM_BASE, b'\x00' * (PITCH * 4))
reg(0x2d0, 0x3)                     # dst 15bpp
reg(0x2d4, 0x70003)
reg(0x2d8, 0x100)
reg(0x2c4, 0x4210)
reg(0x10c, 0)
reg(0x118, (2 << 16) | 1)
r = q.read_bytes(VRAM_BASE, 4)
result("fill15-bigendian", list(r) == [0x42, 0x10, 0x42, 0x10],
       f"got {r.hex()}")

# --- test 8: NOT_DST invert fill (dp_src as the NDRV programs it) ---
base = bytes(range(256)) * (PITCH*4//256 + 1)
q.write_bytes(VRAM_BASE, base[:PITCH*4])
engine_defaults()
reg(0x2d4, 0x0)                     # DP_MIX: frgd=NOT_DST, bkgd=NOT_DST
reg(0x2d8, 0x707)                   # the observed invert dp_src
reg(0x10c, (3<<16)|1)               # (x=3,y=1)
reg(0x118, (5<<16)|2)               # 5x2
ok = True
for yy in range(4):
    r = q.read_bytes(VRAM_BASE + yy*PITCH, 12)
    for xx in range(12):
        orig = base[yy*PITCH + xx]
        e = (~orig) & 0xff if (1<=yy<=2 and 3<=xx<8) else orig
        if r[xx] != e:
            print(f"INVERT MISMATCH ({xx},{yy}): {r[xx]:#x} != {e:#x}"); ok=False
result("invert", ok)

# --- test 9: mono 8x8 pattern fill, documented packing: row r of the
# pattern = byte r (LOW byte first) of PAT_REG0 (rows 0-3) / PAT_REG1
# (rows 4-7); leftmost pixel = MSB of the row byte (BYTE_PIX_ORDER=0).
# Selected the Mac NDRV way: FRGD_SRC=PATTERN + PAT_CNTL bit 24. ---
pat_rows = [0b10000001,0b01000010,0b00100100,0b00011000,
            0b00011000,0b00100100,0b01000010,0b11100001]   # asymmetric row 7
def pat_regs(rows):
    p0 = sum(rows[r] << (8 * r) for r in range(4))
    p1 = sum(rows[r + 4] << (8 * r) for r in range(4))
    return p0, p1
q.write_bytes(VRAM_BASE, b'\x11'*(PITCH*10))
engine_defaults()
reg(0x2c4, 0xF0)                    # frgd
reg(0x2c0, 0x0C)                    # bkgd
reg(0x2d4, 0x70007)                 # frgd=S, bkgd=S
reg(0x2d8, 0x400)                   # FRGD_SRC=PATTERN
reg(0x288, 0x01000000)              # PAT_CNTL: NDRV-style mono enable
p0, p1 = pat_regs(pat_rows)
reg(0x280, p0); reg(0x284, p1)
reg(0x10c, 0)                       # (0,0)
reg(0x118, (16<<16)|10)             # 16x10 -> tiles the 8x8 pattern
ok = True
for yy in range(10):
    r = q.read_bytes(VRAM_BASE + yy*PITCH, 16)
    for xx in range(16):
        bit = (pat_rows[yy & 7] >> (7 - (xx & 7))) & 1
        e = 0xF0 if bit else 0x0C
        if r[xx] != e:
            print(f"PATTERN MISMATCH ({xx},{yy}): {r[xx]:#x} != {e:#x}"); ok=False
result("pattern-ndrv", ok)

# --- test 9b: same pattern via MONO_SRC_PATTERN + PAT_CNTL bit 0 (the
# XFree86 XAA programming model), pattern anchored at screen origin ---
q.write_bytes(VRAM_BASE, b'\x11'*(PITCH*10))
reg(0x2d8, 0x10100)                 # MONO_SRC=PATTERN | FRGD_SRC=FRGD_CLR
reg(0x288, 0x1)                     # PAT_CNTL: PAT_MONO_EN (component bit)
reg(0x10c, (3 << 16) | 2)           # offset rect: pattern stays anchored
reg(0x118, (12<<16)|7)
ok = True
for yy in range(10):
    r = q.read_bytes(VRAM_BASE + yy*PITCH, 16)
    for xx in range(16):
        inside = (2 <= yy < 9 and 3 <= xx < 15)
        bit = (pat_rows[yy & 7] >> (7 - (xx & 7))) & 1
        e = (0xF0 if bit else 0x0C) if inside else 0x11
        if r[xx] != e:
            print(f"PATTERN-XAA MISMATCH ({xx},{yy}): {r[xx]:#x} != {e:#x}"); ok=False
result("pattern-xaa", ok)

# --- test 10: the full bitwise mix table on an 8bpp fill ---
def mix_ref(d, srcv, mix):
    tbl = {
        0x0: ~d, 0x1: 0, 0x2: 0xff, 0x3: d, 0x4: ~srcv, 0x5: d ^ srcv,
        0x6: ~(d ^ srcv), 0x7: srcv, 0x8: ~(d & srcv), 0x9: ~srcv | d,
        0xa: srcv | ~d, 0xb: d | srcv, 0xc: d & srcv, 0xd: srcv & ~d,
        0xe: ~srcv & d, 0xf: ~(d | srcv),
    }
    return tbl[mix] & 0xff
ok = True
engine_defaults()
reg(0x2d8, 0x100)
reg(0x2c4, 0xA5)
for mix in range(16):
    q.write_bytes(VRAM_BASE, bytes([0x3C]) * 8)
    reg(0x2d4, (mix << 16) | 0x3)
    reg(0x10c, (0 << 16) | 0)
    reg(0x118, (4 << 16) | 1)
    r = q.read_bytes(VRAM_BASE, 8)
    e = mix_ref(0x3C, 0xA5, mix)
    for x in range(8):
        expect = e if x < 4 else 0x3C
        if r[x] != expect:
            print(f"MIX {mix:#x} MISMATCH x={x}: {r[x]:#x} != {expect:#x}")
            ok = False
result("mix-table", ok)

# --- test 11: plane write mask ---
q.write_bytes(VRAM_BASE, b'\x55' * 8)
reg(0x2d4, 0x70003)
reg(0x2c4, 0xFF)
reg(0x2c8, 0x0F0F0F0F)              # only low nibble writable
reg(0x10c, 0)
reg(0x118, (4 << 16) | 1)
r = q.read_bytes(VRAM_BASE, 8)
result("write-mask", all(r[i] == (0x5F if i < 4 else 0x55) for i in range(8)),
       f"got {r.hex()}")
reg(0x2c8, 0xffffffff)

# --- test 12: color-compare transparency ---
# 12a: source keying on a copy (fn EQUAL + SRC: source pixels equal to
# the key are not copied -- X's transparent blit)
src = bytes([0xAA, 0x77, 0xAA, 0x33, 0x77, 0xAA])
q.write_bytes(VRAM_BASE, src + b'\x00' * (PITCH - 6) + b'\x11' * PITCH)
reg(0x2d8, 0x300)                   # BLIT
reg(0x300, 0xAA)                    # CLR_CMP_CLR = key
reg(0x304, 0xff)                    # CLR_CMP_MSK
reg(0x308, (1 << 24) | 5)           # SRC_2D | EQUAL -> suppress key pixels
reg(0x18c, 0)                       # SRC (0,0)
reg(0x198, (6 << 16) | 1)
reg(0x10c, (0 << 16) | 1)           # DST (0,1)
reg(0x118, (6 << 16) | 1)
r = q.read_bytes(VRAM_BASE + PITCH, 8)
exp = [0x11, 0x77, 0x11, 0x33, 0x77, 0x11, 0x11, 0x11]
result("clr-cmp-src", all(r[i] == exp[i] for i in range(8)),
       f"got {r.hex()}")
# 12b: destination keying (fn NOT_EQUAL + DST: only key-valued dst
# pixels are writable)
q.write_bytes(VRAM_BASE, bytes([0x20, 0x99, 0x20, 0x99]))
reg(0x2d8, 0x100)
reg(0x2c4, 0xEE)
reg(0x300, 0x20)
reg(0x308, 4)                       # DST | NOT_EQUAL -> suppress dst != key
reg(0x10c, 0)
reg(0x118, (4 << 16) | 1)
r = q.read_bytes(VRAM_BASE, 4)
result("clr-cmp-dst", list(r) == [0xEE, 0x99, 0xEE, 0x99], f"got {r.hex()}")
reg(0x308, 0)

# --- test 13: Bresenham lines, all octants, against a reference ---
def ref_line(x, y, dx, dy, last_pel):
    """software model with the documented register formulas"""
    pts = []
    absdx, absdy = abs(dx), abs(dy)
    ymajor = absdy > absdx
    amaj, amin = (absdy, absdx) if ymajor else (absdx, absdy)
    err = 2 * amin - amaj
    inc = 2 * amin
    dec = 2 * (amin - amaj)
    xd = 1 if dx >= 0 else -1
    yd = 1 if dy >= 0 else -1
    n = amaj + 1 - (0 if last_pel else 1)
    for _ in range(n):
        pts.append((x, y))
        if err < 0:
            err += inc
        else:
            err += dec
            if ymajor: x += xd
            else:      y += yd
        if ymajor: y += yd
        else:      x += xd
    return pts, err, inc, dec, amaj

ok = True
engine_defaults()
reg(0x2d8, 0x100)
reg(0x2c4, 0x77)
cases = [(10, 10, 7, 3), (10, 10, 3, 7), (10, 10, -7, 3), (10, 10, -3, -7),
         (10, 10, 7, -3), (10, 10, -7, -3), (10, 10, 3, -7), (10, 10, -3, 7),
         (5, 5, 9, 0), (5, 5, 0, 9), (8, 8, 6, 6)]
for last_pel in (1, 0):
    for (x0, y0, dx, dy) in cases:
        q.write_bytes(VRAM_BASE, b'\x00' * (PITCH * 24))
        pts, err0, inc, dec, amaj = ref_line(x0, y0, dx, dy, last_pel)
        absdx, absdy = abs(dx), abs(dy)
        ymajor = absdy > absdx
        amin = min(absdx, absdy)
        dst_cntl = (1 if dx >= 0 else 0) | (2 if dy >= 0 else 0) | \
                   (4 if ymajor else 0) | (0x20 if last_pel else 0)
        reg(0x130, dst_cntl)
        reg(0x10c, (x0 << 16) | y0)
        reg(0x124, (2 * amin - amaj) & 0x3ffff)   # ERR (signed 18-bit)
        reg(0x128, (2 * amin) & 0x3ffff)          # INC
        reg(0x12c, (2 * (amin - amaj)) & 0x3ffff) # DEC
        reg(0x120, amaj + 1)                      # LNTH -> draw
        fb = q.read_bytes(VRAM_BASE, PITCH * 24)
        want = set(pts)
        for yy in range(24):
            for xx in range(24):
                e = 0x77 if (xx, yy) in want else 0x00
                if fb[yy * PITCH + xx] != e:
                    print(f"LINE ({dx},{dy},lp={last_pel}) MISMATCH at "
                          f"({xx},{yy}): {fb[yy*PITCH+xx]:#x} != {e:#x}")
                    ok = False
reg(0x130, 0x3)
result("bres-lines", ok)

# --- test 14: mono expansion from a VRAM blit source (MONO_SRC=BLIT) ---
# 1bpp source bitmap at VRAM offset 0x8000, 2 rows, src_pitch 64 px(bits)
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 4))
srcbits = bytes([0b10110001, 0b01001110])       # row0 byte, row1 byte
q.write_bytes(VRAM_BASE + 0x8000, srcbits[0:1] + b'\x00' * 7 + srcbits[1:2])
engine_defaults()
reg(0x2c4, 0xEE); reg(0x2c0, 0x22)
reg(0x2d4, 0x70007)
reg(0x2d8, 0x30100)                 # MONO_SRC=BLIT | FRGD_SRC=FRGD_CLR
reg(0x180, ((64 // 8) << 22) | (0x8000 // 8))   # SRC_OFF_PITCH: 64px pitch
reg(0x18c, 0)                       # SRC_Y_X (0,0)
reg(0x10c, (4 << 16) | 1)
reg(0x118, (8 << 16) | 2)
ok = True
for yy in range(4):
    r = q.read_bytes(VRAM_BASE + yy * PITCH, 16)
    for xx in range(16):
        if yy in (1, 2) and 4 <= xx < 12:
            e = 0xEE if (srcbits[yy - 1] >> (7 - (xx - 4))) & 1 else 0x22
        else:
            e = 0x11
        if r[xx] != e:
            print(f"MONOBLIT MISMATCH ({xx},{yy}): {r[xx]:#x} != {e:#x}")
            ok = False
result("mono-blit", ok)

# --- test 15: DP_SET_GUI_ENGINE2 macro programs a whole fill context
# (the exact shape Apple's NDRV uses; pitch code 4 = 640) ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 4))
reg(0x100, 0)                       # macro must supply the pitch
reg(0x308, (1 << 24) | 5)           # stale compare: macro must clear it
reg(0x2c8, 0)                       # stale write mask: bit22 must reset it
# [31] src=dst offpitch, [30:27]=4 (pitch 640), [26] src width = dst,
# [25:23]=2 (8bpp), [22] write mask, [17][16] T2B/L2R,
# frgd_src=FRGD_CLR, mixes S/S
e2 = (1 << 31) | (4 << 27) | (1 << 26) | (2 << 23) | (1 << 22) | \
     (3 << 16) | (1 << 11) | 0x77
reg(0x2f8, e2)
reg(0x2c4, 0x5A)
reg(0x10c, (2 << 16) | 1)
reg(0x118, (4 << 16) | 2)
r0 = q.read_bytes(VRAM_BASE + 0 * PITCH, 8)
r1 = q.read_bytes(VRAM_BASE + 1 * PITCH, 8)
ok = all(r1[i] == (0x5A if 2 <= i < 6 else 0x11) for i in range(8)) and \
     all(b == 0x11 for b in r0)
result("engine2-macro", ok, f"got {r1.hex()}")

# --- test 16: GUI_TRAJ_CNTL decomposes into its component registers
# (atyfb writes 0x100023 at engine init and depends on it) ---
reg(0x330, (1 << 28) | (1 << 27) | (0x10 << 16) | 0x23)
ok = (regread(0x130) == 0x23 and       # DST_CNTL: L2R|T2B|LAST_PEL
      regread(0x1b4) == 0x10 and       # SRC_CNTL: LINE_X_L2R
      regread(0x240) == 0x3)           # HOST_CNTL: ALIGN|BIG_ENDIAN
result("gui-traj-decompose", ok,
       f"dst={regread(0x130):#x} src={regread(0x1b4):#x} "
       f"host={regread(0x240):#x}")
reg(0x330, 0x3)                     # restore plain L2R/T2B

# --- test 17: GUI bus master: descriptor-table DMA feeding HOST_DATA
# (the way the accelerated Mac OS NDRV uploads all image data) ---
RAM_DATA, RAM_TABLE = 0x200000, 0x201000
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 4))
engine_defaults()
reg(0x2d0, 0x20002)                 # dst 8bpp | host 8bpp
reg(0x2d4, 0x70007)
reg(0x2d8, 0x200)                   # FRGD_SRC=HOST
reg(0x240, 0x0)
reg(0x10c, (2 << 16) | 1)           # (x=2,y=1)
reg(0x118, (16 << 16) | 2)          # 16x2 -> arm host blit (32 bytes)
pix = bytes((0xA0 + i) & 0xff for i in range(32))
q.write_bytes(RAM_DATA, pix)
# one descriptor: fb offset = HOST_DATA0's aperture offset, HOLD, EOL
desc = struct.pack('<4I', 0x7FFC00 + 0x200, RAM_DATA,
                   32 | (1 << 30) | (1 << 31), 0)
q.write_bytes(RAM_TABLE, desc)
reg(0x24c, RAM_TABLE)               # BM_GUI_TABLE_CMD -> kick
r1 = q.read_bytes(VRAM_BASE + 1 * PITCH + 2, 16)
r2 = q.read_bytes(VRAM_BASE + 2 * PITCH + 2, 16)
ok = r1 == pix[:16] and r2 == pix[16:]
intr = regread(0x18)
ok_int = bool(intr & (1 << 25))
reg(0x18, intr)         # ack
ok_ack = not (regread(0x18) & (1 << 25))
result("busmaster-hostdata", ok and ok_int and ok_ack,
       f"rows {r1.hex()}/{r2.hex()} int={intr:#x}")

# --- test 17b: bus master linear VRAM write (no HOLD), kicked through
# the block-1 register window inside the framebuffer aperture ---
q.write_bytes(VRAM_BASE + 0x4000, b'\x00' * 32)
q.write_bytes(RAM_DATA, bytes(range(32)))
desc = struct.pack('<4I', 0x4000, RAM_DATA, 32 | (1 << 31), 0)
q.write_bytes(RAM_TABLE, desc)
# BM_GUI_TABLE (block-1 reg 0x5B8) via the 0x7FF800 aperture window
q.writel_le(VRAM_BASE + 0x7FF800 + 0x1B8, RAM_TABLE)
r = q.read_bytes(VRAM_BASE + 0x4000, 32)
result("busmaster-vram-blk1", r == bytes(range(32)), f"got {r.hex()}")

# --- test 17c: BM_ADDR/BM_DATA register-list port ---
q.write_bytes(VRAM_BASE, b'\x11' * 64)
# addr mode: reg 0xB1 = DP_FRGD_CLR(0x2C4>>2), count field 0 -> 1 reg
reg(0x248, 0xB1)
reg(0x248, 0x00000077)              # data -> DP_FRGD_CLR
ok = regread(0x2c4) == 0x77
result("busmaster-reglist", ok, f"frgd={regread(0x2c4):#x}")

# --- test 18: source trajectories (Programmer's Guide 2-40..2-42) ---
# a 16x8 tile at (0,0) tiled across a wider/taller destination
engine_defaults()
tile_w, tile_h = 16, 8
tile = bytes(((y * 16 + x * 3) & 0xff) or 1 for y in range(tile_h) for x in range(PITCH))
q.write_bytes(VRAM_BASE, tile)                      # tile lives at src (0,0)
def run_blit(src_cntl, dst_y, w, h, xs=0, ys=0):
    q.write_bytes(VRAM_BASE + dst_y * PITCH, b'\x00' * (PITCH * h))
    reg(0x1b4, src_cntl)                            # SRC_CNTL
    reg(0x198, (tile_w << 16) | tile_h)             # SRC_HEIGHT1_WIDTH1
    reg(0x1a4, (xs << 16) | ys)                     # SRC_Y_X_START
    reg(0x18c, 0)                                   # SRC_Y_X = (0,0)
    reg(0x2d8, 0x300)                               # FRGD_SRC = BLIT
    reg(0x10c, (0 << 16) | dst_y)
    reg(0x118, (w << 16) | h)                       # trigger
    return [q.read_bytes(VRAM_BASE + (dst_y + r) * PITCH, w) for r in range(h)]

# trajectory 3: pattern, wraps in X and Y
rows = run_blit(0x01, 300, 40, 20)
ok = all(rows[r][c] == tile[(r % tile_h) * PITCH + (c % tile_w)]
         for r in range(20) for c in range(40))
result("src-traj3-pattern", ok, f"row0={rows[0][:20].hex()}")

# trajectory 2 (no SRC_CNTL bits): we deliberately do NOT wrap at
# SRC_WIDTH1 here -- drivers leave a stale SRC_WIDTH1 from an earlier
# pattern blit, and wrapping ordinary copies on it smears window text
# and breaks scrolling. A plain rectangular copy is expected.
rows = run_blit(0x00, 340, 40, 6)
ok = all(rows[r][c] == tile[r * PITCH + c] for r in range(6) for c in range(40))
result("src-traj2-plain-rect", ok, f"row0={rows[0][:20].hex()}")

# trajectory 4: pattern with rotation (start phase)
rows = run_blit(0x03, 360, 40, 20, xs=5, ys=3)
ok = all(rows[r][c] == tile[((r + 3) % tile_h) * PITCH + ((c + 5) % tile_w)]
         for r in range(20) for c in range(40))
result("src-traj4-rotated", ok, f"row0={rows[0][:20].hex()}")

# trajectory 1: strictly linear source consumption
rows = run_blit(0x04, 380, 32, 4)
flat = tile[:0]  # linear stream starts at src offset 0
ok = all(rows[r][c] == tile[(r * 32 + c)] for r in range(4) for c in range(32)
         if (r * 32 + c) < PITCH)
result("src-traj1-linear", ok, f"row0={rows[0][:20].hex()}")
reg(0x1b4, 0)                                       # restore plain source

print(f"\n{npass} passed, {nfail} failed")
sys.exit(1 if nfail else 0)
