# HANDOFF: Rage 128 3D step 4 -- texture mapping, plus the per-pixel overhead

Contract for a reader with zero shared context. Read `SURFACE.md` and
`HANDOFF-rasterizer.md` in this directory first; the latter's conventions
(files, build, qtest harness, hard rules) all still apply.

## Where things stand (verified 2026-09-02 evening)

- Commits on branch `g3beige` of `/Users/hsp/src/claude-code/qemu-master-g3`:
  `08545fe458` (GEN_PRIM decode), `cb9aa608be` (Gouraud/Z/scissor rasterizer,
  `ati_rage128_3d_triangle()` in `hw/display/ati_rage128_2d.c`, harness
  `raster_regress.py` 6/6), `46db9a1d64` (MULTI-packet context prefix fix --
  read its message: it is why the scene was black).
- LIVE RESULT (user, Nanosaur 1.1.6 on Mac OS 9.2): the 3D scene renders --
  the Pangea logo spins and the game shows -- in BLACK AND WHITE, because no
  texturing exists yet, and VERY SLOWLY.
- PROFILE (macOS `sample`, 5 s, live spin): the vCPU thread spends ~77% of
  its time inside the rage128 packet parser: 2D blits (per-frame clears +
  back->front presentation copies, `ati_rage128_2d_blt`) 1209 samples,
  the rasterizer 811. Hot leaves are per-pixel dirty marking and VRAM
  pointer re-resolution: `physical_memory_set_dirty_range`,
  `qemu_ram_ptr_length`, `get_ptr_rcu_reader`, `bitmap_set_atomic`,
  `_tlv_get_addr`. Both paths go through `ati_rage128_2d_read/write_pixel`
  helpers that mark dirty and re-derive the pointer per pixel.

## Goal A -- texture mapping (functional, first priority)

Make Nanosaur's textured triangles come out textured. What is known:

- Every triangle carries s,t (0..1 floats) and rhw per vertex (vc_format
  0xa7, see SURFACE.md); do perspective-correct interpolation using rhw
  (interpolate s/w, t/w, 1/w linearly, divide per pixel).
- State registers, all latched in `s->regs[]` (values Nanosaur programs):
  - `TEX_CNTL_C` 0x1c9c: 0x193 (dominant), 0x183, 0x2193, 0x800193. Bits per
    Linux DRM `r128_reg.h` (reference copy:
    `/Volumes/Macdata/qemu/doc/rage128-cce/r128_reg.h`; copy needed defines
    into `ati_rage128_regs.h`): Z_ENABLE, Z_WRITE_ENABLE, TEXMAP_ENABLE,
    SEC_TEXMAP_ENABLE, alpha test, dither... -- decode which bit enables
    texturing and gate on it (0x183 vs 0x193 differ in bit 4).
  - `PRIM_TEX_CNTL_C` 0x1cb0: 0x01030080, 0x010300b6, 0x010312b6,
    0x01060080 -- texture format (datatype field), min/mag filter, clamp/
    wrap, mip levels. Decode against r128_reg.h's `R128_PRIM_TEX_CNTL_C`
    field defines (DATATYPE, MIN_BLEND, MAG_BLEND, CLAMP_S/T, ...).
  - `PRIM_TEXTURE_COMBINE_CNTL_C` 0x1cb4: 0x0418d040 / 0x0418d043 -- how the
    texel combines with the Gouraud colour (modulate/decal/...); start with
    MODULATE if the decode says so.
  - `TEX_SIZE_PITCH_C` 0x1cb8: 0x03330888 (and 0x03330333) -- texture width/
    height/pitch shifts and the mip-level count.
  - `PRIM_TEX_0..10_OFFSET_C` 0x1cbc..0x1ce4: per-mip-level texture base
    offsets in VRAM. **CAUTION:** the morning corpus showed float-looking
    values in some of these slots (0x3f800000, 0x437f0000 ...). Before
    trusting r128_reg.h's assignment for the Pro, confirm each offset in
    the RRG text (`/Volumes/Macdata/qemu/doc/rage128-cce/
    rage128-RRG-G04500-C-text.txt`, grep the hex offset) -- the register
    map in that range may differ from the non-Pro DRM header. Do this
    check FIRST and write down what you found.
  - `SEC_TEX_CNTL_C` 0x1d00 (0x01038081) / `SEC_TEX_COMBINE_CNTL_C` 0x1d04 /
    `SEC_TEX_n_OFFSET_C`: second texture unit -- OUT OF SCOPE for this step
    unless the picture proves it is needed; note what it is set to.
  - Textures live in VRAM already (uploaded by the guest via HOSTDATA_BLT,
    which the model handles); sample them from
    `memory_region_get_ram_ptr(&s->vram)` with every address bounds-checked
    (offsets/pitches/sizes are guest-controlled).
- Formats to support: whatever the datatype field says among ARGB1555,
  RGB565, ARGB8888, ARGB4444 (RAVE-era games use 1555 and 4444 for alpha).
  Unknown format -> trace once, draw untextured.
- Filtering: nearest is acceptable for this step; bilinear if cheap. One
  mip level (level 0) is acceptable; note if the game selects others.
- Alpha: apply alpha TEST if `TEX_CNTL_C` enables it (the HUD/sprites will
  need it -- `SCALE_3D_CNTL` holds the alpha-test function per SURFACE.md);
  alpha BLENDING (`MISC_3D_STATE_CNTL_REG` 0x00510200 / 0x00540200) is
  optional here -- implement only if straightforward, else leave a seam.

Acceptance for A (harness, `raster_regress.py`, extend it): a known texture
(e.g. 8x8 checkerboard in ARGB1555) placed in VRAM, registers programmed,
one textured triangle drawn -> assert texel colours land at the expected
pixels (both checker colours present, exact values, correct orientation --
a flipped-t bug is the classic error, test it explicitly with an asymmetric
texture), plus a control with texturing disabled -> Gouraud colour only.
All previous 6 checks must still pass.

## Goal B -- per-pixel overhead (measured, second priority)

Remove the per-pixel dirty-mark + pointer-resolve cost from BOTH the 2D
blitter's row loops and the rasterizer's scanline loop:
- resolve the VRAM base pointer once per draw; write pixels directly with
  explicit bounds checks hoisted per row/span;
- call `memory_region_set_dirty()` once per row (or once per bounding box
  per draw), not per pixel.
Keep `ati_rage128_2d_read/write_pixel` for the odd paths; do not change any
rendered byte (harness stays byte-identical; extend it with a checksum of
the whole target after each check so this is proven, not assumed).

Acceptance for B: a qtest benchmark (`raster_bench.py`, same harness base)
that pushes a fixed workload -- e.g. 2000 frames of [PAINT_MULTI clear
640x480] + [200 textured/gouraud triangles ~2000 px each] + [BITBLT_MULTI
640x480 present] -- and reports wall-clock seconds BEFORE and AFTER (run
each 3x, report all numbers). Report the speedup as measured; no claims
without the numbers.

## Rules (unchanged from HANDOFF-rasterizer.md)

Work only in this tree; no commits, no pushes; do not boot guest OSes (the
qtest harness is your runtime); another agent is working in hw/scsi of the
same tree and builds it -- if a build fails in files you did not touch,
wait and retry. Scratch files in
`/private/tmp/claude-502/-Users-hsp-src-claude-code-qemu-master-g3/95e0d9a0-e724-4369-807f-4acdfe15873f/scratchpad/tex/`
(create it); short unix socket paths under /tmp.

## Report back

Per goal: DONE / PARTIAL / NOT RUN with reason; the register decodes you
established (with the RRG/DRM lines they come from); every harness check
with its measured numbers; the benchmark numbers; deviations from this
contract stated plainly.
