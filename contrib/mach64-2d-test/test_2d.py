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
# enable memory decode
cfg_write(ati, 4, cfg_read(ati, 4) | 2)
# check BARs landed
print("BAR0:", hex(cfg_read(ati, 0x10)), "BAR2:", hex(cfg_read(ati, 0x18)))

R = MMIO_BASE
def reg(off, val):
    q.writel_le(R + off, val)

PITCH = 640
# --- test 1: 8bpp solid fill 10x4 at (5,3) ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 16))
reg(0x2d0, 0x2)                     # DP_PIX_WIDTH: dst 8bpp
reg(0x2d8, 0x100)                   # DP_SRC: frgd = FRGD_CLR, mono=one
reg(0x2d4, 0x70003)                 # DP_MIX: frgd=S, bkgd=D
reg(0x2c4, 0xAB)                    # DP_FRGD_CLR
reg(0x100, (PITCH // 8) << 22)      # DST_OFF_PITCH: offset 0
reg(0x130, 0x3)                     # DST_CNTL: L2R, T2B
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
print("fill test:", "PASS" if ok else "FAIL")

# --- test 2: overlapping copy downward (scroll down by 2 rows) ---
pat = bytes((y * 37 + x * 11) & 0xff for y in range(12) for x in range(PITCH))
q.write_bytes(VRAM_BASE, pat)
reg(0x2d8, 0x300)                   # DP_SRC: frgd = BLIT
reg(0x180, (PITCH // 8) << 22)      # SRC_OFF_PITCH
reg(0x18c, (0 << 16) | 0)           # SRC_Y_X = (0,0)
reg(0x198, (PITCH << 16) | 10)      # SRC_HEIGHT1_WIDTH1
reg(0x10c, (0 << 16) | 2)           # DST_Y_X = (0,2)
reg(0x118, (PITCH << 16) | 10)      # DST -> trigger: copy 640x10 down 2
ok = True
after = q.read_bytes(VRAM_BASE, PITCH * 12)
for y in range(12):
    for x in range(0, PITCH, 53):
        if y < 2:
            expect = pat[y * PITCH + x]
        else:
            expect = pat[(y - 2) * PITCH + x]
        got = after[y * PITCH + x]
        if got != expect:
            print(f"COPY MISMATCH at ({x},{y}): {got:#x} != {expect:#x}")
            ok = False
print("overlap copy test:", "PASS" if ok else "FAIL")

# --- test 3: scissored fill ---
q.write_bytes(VRAM_BASE, b'\x22' * (PITCH * 16))
reg(0x2a8, (12 << 16) | 8)          # SC_LEFT_RIGHT: left=8 right=12
reg(0x2b4, (9 << 16) | 6)           # SC_TOP_BOTTOM: top=6 bottom=9
reg(0x2d8, 0x100)
reg(0x2c4, 0xCD)
reg(0x10c, (0 << 16) | 0)
reg(0x118, (100 << 16) | 16)        # huge fill -> must clip to scissors
ok = True
for y in range(16):
    row = q.read_bytes(VRAM_BASE + y * PITCH, 20)
    for x in range(20):
        expect = 0xCD if (6 <= y <= 9 and 8 <= x <= 12) else 0x22
        if row[x] != expect:
            print(f"SCISSOR MISMATCH at ({x},{y}): {row[x]:#x} != {expect:#x}")
            ok = False
print("scissor test:", "PASS" if ok else "FAIL")

# --- test 4: 16bpp XOR fill ---
q.write_bytes(VRAM_BASE, b'\x0f\xf0' * (PITCH * 4))
reg(0x2a8, 0x1fff0000)              # scissors open
reg(0x2b4, 0x7fff0000)
reg(0x2d0, 0x4)                     # 16bpp
reg(0x2d4, 0x50003)                 # frgd mix = XOR
reg(0x2c4, 0xFFFF)
reg(0x100, (PITCH // 8) << 22)
reg(0x10c, (2 << 16) | 1)
reg(0x118, (4 << 16) | 2)           # 4x2 pixels at (2,1)
ok = True
for y in range(4):
    row = q.read_bytes(VRAM_BASE + y * PITCH * 2, 16)
    for x in range(8):
        inside = (1 <= y <= 2 and 2 <= x <= 5)
        expect = (0xf0f0 ^ 0xffff) if inside else 0xf00f
        got = struct.unpack('<H', row[x*2:x*2+2])[0]
        exp_bytes = struct.unpack('<H', bytes([0x0f ^ (0xff if inside else 0), 0xf0 ^ (0xff if inside else 0)]))[0]
        if got != exp_bytes:
            print(f"XOR MISMATCH at ({x},{y}): {got:#x} != {exp_bytes:#x}")
            ok = False
print("xor16 test:", "PASS" if ok else "FAIL")

# --- test 5: right-to-left / bottom-to-top copy (corner-start coords) ---
pat = bytes((y * 7 + x * 3) & 0xff for y in range(12) for x in range(PITCH))
q.write_bytes(VRAM_BASE, pat)
reg(0x2d0, 0x2)                     # back to 8bpp
reg(0x2d4, 0x70003)                 # mix S
reg(0x2d8, 0x300)                   # BLIT
reg(0x130, 0x0)                     # DST_CNTL: R2L, B2T -> coords are bottom-right corner
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
print("direction test:", "PASS" if ok else "FAIL")

# --- test 6: 8bpp mono expansion host blit (glyph draw) ---
# 8x2 rect at (4,3); fg=0xEE (1-bits), bg=0x22 (0-bits), both mix S (opaque)
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 8))
reg(0x2d0, 0x2)                     # DP_PIX_WIDTH: dst 8bpp, host 1bpp(mono) (bits16-19=0)
reg(0x2c4, 0xEE)                    # DP_FRGD_CLR
reg(0x2c0, 0x22)                    # DP_BKGD_CLR
reg(0x2d4, 0x70007)                 # DP_MIX: frgd=S(0x70000), bkgd=S(0x7)
reg(0x2d8, 0x20100)                 # DP_SRC: MONO_SRC=HOST(0x20000) | FRGD_SRC=FRGD_CLR(0x100)
reg(0x240, 0x1)                     # HOST_CNTL: BYTE_ALIGN
reg(0x100, (PITCH // 8) << 22)      # DST_OFF_PITCH
reg(0x130, 0x3)                     # DST_CNTL L2R T2B
reg(0x10c, (4 << 16) | 3)           # DST_Y_X
reg(0x118, (8 << 16) | 2)           # DST_HEIGHT_WIDTH -> arm host blit
# stream 2 words, MSB-first: row0 = 0b10110001..., row1 = 0b01001110...
row0 = 0b10110001
row1 = 0b01001110
reg(0x200, row0 << 24)              # HOST_DATA0: bits 31..24 = the 8 pixels
reg(0x200, row1 << 24)
ok = True
def expect_mono(bits, x):
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
print("mono-host test:", "PASS" if ok else "FAIL")

# --- test 7: 8bpp packed-color host blit ---
q.write_bytes(VRAM_BASE, b'\x11' * (PITCH * 8))
reg(0x2d0, 0x20002)                 # dst 8bpp | host 8bpp(bits16-19=2 -> 0x20000)
reg(0x2d4, 0x70007)
reg(0x2d8, 0x200)                   # FRGD_SRC=HOST(0x200), MONO_SRC=ONE
reg(0x240, 0x1)
reg(0x100, (PITCH // 8) << 22)
reg(0x130, 0x3)
reg(0x10c, (2 << 16) | 1)           # (x=2,y=1)
reg(0x118, (4 << 16) | 1)           # 4x1
# one word = 4 packed 8bpp pixels, low pixel first: p0=0xA0,p1=0xB1,p2=0xC2,p3=0xD3
reg(0x200, 0xD3C2B1A0)
r = q.read_bytes(VRAM_BASE + 1 * PITCH, 8)
exp = [0x11,0x11,0xA0,0xB1,0xC2,0xD3,0x11,0x11]
ok = all(r[i]==exp[i] for i in range(8))
print("color-host test:", "PASS" if ok else "FAIL", "" if ok else f"got {[hex(b) for b in r[:8]]}")

# --- test 8: NOT_DST invert fill ---
base = bytes(range(256))*  (PITCH*4//256 + 1)
q.write_bytes(VRAM_BASE, base[:PITCH*4])
reg(0x2d0, 0x2)                     # 8bpp
reg(0x2d4, 0x0)                     # DP_MIX: frgd=NOT_DST(0), bkgd=NOT_DST(0)
reg(0x2d8, 0x707)                   # the observed invert dp_src
reg(0x2a8, 0x1fff0000); reg(0x2b4, 0x7fff0000)  # scissors open
reg(0x100, (PITCH//8)<<22); reg(0x130, 0x3)
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
print("invert test:", "PASS" if ok else "FAIL")

# --- test 9: mono 8x8 pattern fill ---
q.write_bytes(VRAM_BASE, b'\x11'*(PITCH*10))
reg(0x2d0, 0x2)
reg(0x2c4, 0xF0)                    # frgd
reg(0x2c0, 0x0C)                    # bkgd
reg(0x2d4, 0x70007)                 # frgd=S, bkgd=S
reg(0x2d8, 0x400)                   # FRGD_SRC=PATTERN
reg(0x288, 0x01000000)             # PAT_CNTL: MONO_8x8_ENABLE
# pattern: rows0-3 in PAT_REG0, rows4-7 in PAT_REG1; 8 bits/row MSB-left
pat_rows = [0b10000001,0b01000010,0b00100100,0b00011000,
            0b00011000,0b00100100,0b01000010,0b10000001]
pat0 = sum(pat_rows[r] << ((3-r)*8) for r in range(4))
pat1 = sum(pat_rows[r] << ((3-(r-4))*8) for r in range(4,8))
reg(0x280, pat0); reg(0x284, pat1)
reg(0x100, (PITCH//8)<<22); reg(0x130, 0x3)
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
print("pattern test:", "PASS" if ok else "FAIL")
