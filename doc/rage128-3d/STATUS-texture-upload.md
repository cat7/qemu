# STATUS: Nanosaur's black textures -- the upload was caught (2026-09-02, late)

For a reader with zero shared context. Contract: `HANDOFF-texture-upload.md`
in this directory. Everything below was MEASURED in one boot of the user's
config (id=rage, whole-VRAM fillwatch); the fix is in the working tree,
UNCOMMITTED, built, and proven by the qtest harness but NOT yet seen live.

## Verdict in one line

The RAVE driver uploads every texture with a `HOSTDATA_BLT` whose header is
NINE dwords (its GMC announces DST_PITCH_OFFSET *and* DST_CLIPPING); the
model fixed the header at eight, read DST_Y_X as the size (width 0) and
dropped every texel. Nothing was ever written to the texture heap, so VRAM
was zero under *every* offset reading -- the 24-bit reading committed in
`6603c7dd6f` was a misdiagnosis on stale heap, and the DRM's 30-bit reading
(`raw & 0x3fffffff`) is restored.

## The measured chain (trace `scratchpad/texup/upload1.log.gz`, 232 MB raw)

Boot: `build-g3/qemu-system-ppc -M g3beige ... -device ati-rage128-pro,
id=rage,addr=0x0e,romfile=...,fillwatch=0,fillwatch-size=0x2000000`
(the `fillwatch` property lays an instrumented IO window over aperture 0;
covering all 32 MB did NOT break the boot). Launch script:
`scratchpad/texup/drive.py` (QMP; `shot NAME rage` screendumps the card).

Facts that cost the previous pass hours, now settled:

- **Nanosaur auto-launches** in `nano.img` (startup item): ~180 s after boot
  the rage display shows its main menu (`texup/r3.png`). No Finder
  navigation needed. Space must be HELD >= 0.5 s (`tap('spc', hold=0.5)`);
  an 80 ms tap is missed. One space from the menu goes straight into the
  arena (`texup/g3.png`, HUD textured by 2D blits, terrain flat/black).
- This boot's front buffer is VRAM 0x8000 (CRTC_OFFSET), 640x480x16, pitch
  640 -- the earlier "HUD to dst 0x8000" WAS the screen.
- Aperture 0: **0 fillwatch events** for the whole level load and 15 s of
  play -- the CPU never stores through the raw aperture. Aperture 1: 17,370
  writes, all in 0x010000-0x09ffff (screen). HOST_DATA registers: 0.
  Bus-master descriptors: 0. Unimplemented packets: 0. 3D "unsupported": 0.
- **The upload.** During the level load (between `g2.png` and `g3.png`):

  ```
  packet3 opcode=0x94 count=8201   x35   (128x128 ARGB1555 = 8192 dw + 9)
  packet3 opcode=0x94 count=2057   x4    (64x64  ARGB1555 = 2048 dw + 9)
  packet3 opcode=0x94 count=4105   x3    (64x64 ARGB8888 / 128x64 = 4096 + 9)
  packet3 opcode=0x94 count=16137  x2  + count=521 x1
                                        (256x256: 16128+16128+512 = 32768 dw)
  packet3 opcode=0x94 count=968    x160  (HUD text 640x3 = 960 dw + 8)
  ```
  and for each texture packet the model logged
  ```
  ati_rage128_ctx_write 2D context: ? (0x146c) <- 0x53cc33fa
  ati_rage128_2d_blt blt ... wh=0x0 rop=0xcc dt=0x20030f03 srcsel=3 srcoff=0x8000 dstoff=0x1b48000
  ```
  i.e. GMC 0x53cc33fa (bits 1 and 3 set: DST_PITCH_OFFSET + DST_CLIPPING),
  width 0 (`wh=0x0`, `wh=0x126`, `wh=0x252` -- the parser took DST_Y_X as
  DST_HEIGHT_WIDTH), destination the stale back buffer 0x1b48000. The HUD
  blits (GMC 0x73cc33f8, bit 3 only) parse fine with eight; their raw
  header, found in the RAM dump, is
  `73cc33f8 00000000 01df027f 002c0009 80000000 01830000 00030280 000003c0`
  = [GMC][SC_TOP_LEFT][SC_BOTTOM_RIGHT][fixed][fixed][DST_Y_X]
  [DST_HEIGHT_WIDTH][count]. So the header is `1 + gmc_prefix_len + 5`:
  7 (Linux DRM, bit 1), 8 (Mac HUD, bit 3), 9 (RAVE textures, bits 1+3).
  The count identity `hdr[nhdr-1] == count - nhdr` holds for all three.
- Texture offsets named by the 3D context during play (`ati_rage128_3d_tex`,
  28 distinct): raw 0xc1996600 (150,196 triangles), 0xc1ad9600, 0xc1aae900,
  ... 0xc1bd4900; sizes 128x128 (most), 64x64, one 256x256; dt=3 (1555),
  one 64x64 dt=6 (8888). Under `& 0x3fffffff` they span 0x1996600-0x1bd4900
  = 25.6-27.8 MB, directly below the render targets (Z 0x1ae8000, back
  0x1b48000, 0x1bd8000, 0x1c72000) -- a top-down heap. Under `& 0xffffff`
  they would sit at 9.6-11.8 MB, an empty region. Both regions read all
  zero in the arena VRAM dump (`texup/vram_arena.bin`, 64 KB occupancy map
  in the session log) -- as they must, since the texels were dropped.
  The texture packets' own DST_PITCH_OFFSET dwords could not be recovered
  from the RAM dump (the IBs had been recycled), so the 30-bit choice
  rests on the heap layout plus the harness; the live confirmation
  (dstoff of `srcsel=3` blits == 3d_tex raw & 0x3fffffff) is next-step 2.

## The fix (working tree, uncommitted, builds clean)

- `hw/display/ati_rage128.c`: both HOSTDATA_BLT parsers (ring at
  `R128_PM4_OPCODE_HOSTDATA_BLT` in the ring loop, streamed/FIFO in
  `ati_rage128_pm4_parse`) derive the header length from the GMC prefix via
  `ati_rage128_gmc_prefix_len()`, program the prefix registers after the
  context dword (so DST_PITCH_OFFSET sets the destination), take DST_Y_X /
  DST_HEIGHT_WIDTH from `hdr[nhdr-3]` / `hdr[nhdr-2]`, and trace
  `ati_rage128_hostdata_hdr` when the count identity fails or the header
  would exceed the packet.
- `hw/display/ati_rage128_int.h`: `R128_HOSTDATA_HDR_MAX` (11);
  `p3_params[]` sized by it.
- `hw/display/ati_rage128_regs.h`: `R128_TEX_OFFSET_MASK` 0x00ffffff ->
  0x3fffffff, comment rewritten with the above.
- `hw/display/ati_rage128_2d.c`: `ati_rage128_3d_tex` trace now also prints
  the raw offset register. `hw/display/trace-events`: that, plus
  `ati_rage128_hostdata_hdr`.
- `doc/rage128-3d/raster_regress.py`: check15 moved to the HUD form (GMC
  bit 3, destination from DEFAULT_OFFSET/PITCH); new check17 = the measured
  chain (nine-dword upload with GMC 0x53cc33fa / DP_DATATYPE 0x20030f03 to
  0x1996600, then a quad textured through PRIM_TEX_7_OFFSET_C=0xc1996600).

Build: `ninja -C build-g3 qemu-system-ppc` -- clean (run after the last
edit). Harness (`texup/regress_after.txt`): **30/34**.
- PASS (new): "texture upload lands at the 30-bit DST_PITCH_OFFSET target:
  16384/16384 texels exact at 0x1996600, 0 stray bytes at the 24-bit alias";
  "textured quad through PRIM_TEX_7_OFFSET_C = 0xc1996600: 16384/16384
  pixels texel-exact, 16384 painted". CRC 0x17d79d5f (no golden yet).
- FAIL "corpus replay (textured)" + its CRC: the check's FIXTURE places the
  texture at `texbase = 0x00D0AE00` (the 24-bit reading of the corpus
  value 0xC1D0AE00); the device now fetches at 0x01D0AE00. Fixture, not
  device.
- FAIL "2d hostdata blit coverage" + its CRC (0 painted): check15's
  SC_BOTTOM_RIGHT dword 0x3FFF3FFF is now honoured (bit-3 form) and the
  scissors are signed 14-bit, so 0x3fff = -1 clips everything; the live
  HUD packets carry 0x01df027f. Fixture, not device (unverified beyond
  that reasoning -- stop order arrived here).

Live result with the fix: NOT RUN (stop order). The only live run was
with the OLD build (textures black, `texup/g3.png` / `g4.png`).

## Precise next steps, in order

1. In `raster_regress.py`: check5 `texbase = 0x01D0AE00`; check15
   SC_BOTTOM_RIGHT `0x01DF027F` (or 0x1FFF1FFF). Re-run; expect 34/34 with
   the four goldens unchanged; record check17's golden.
2. Boot 2 (same recipe as `texup/drive.py`'s socket `/tmp/texup.sock`,
   fillwatch not needed): arm `ati_rage128_2d_blt ati_rage128_hostdata_hdr
   ati_rage128_3d_tex` before the menu appears (~150 s), hold space 0.5 s,
   then check (a) `srcsel=3` blits now show real `wh=128x128` and
   `dstoff` values, (b) the set of those dstoffs equals the set of
   `3d_tex raw & 0x3fffffff`, (c) `screendump device=rage` shows textured
   terrain. If texels look scrambled/striped, revisit bits 31:30 (DRM calls
   0x3 "TILED_BY_STORAGE2") -- but the upload is a linear HOSTDATA_BLT with
   pitch 128, so linear is the expectation.
3. Then commit (message: header length from the GMC prefix; 24-bit mask
   reverted with the reason). Delete `texup/vram_*.bin` and
   `upload1.log.gz` when no longer needed.
