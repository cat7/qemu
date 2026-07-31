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
