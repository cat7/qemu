# HANDOFF: Rage 128 software rasterizer, step 3 (Gouraud + Z + scissor)

Contract for a reader with zero shared context. Everything needed is in this
tree or stated here. Read `SURFACE.md` in this directory first — its "Corpus
status (2026-09-02, second pass)" section is the ground truth this work builds
on; do not re-derive or second-guess it.

## Goal

Nanosaur (Mac OS 9, RAVE) submits a perfectly healthy 3D triangle stream to
the emulated rage128 and the screen stays black because nothing rasterizes it.
Implement untextured Gouraud triangle rasterization into VRAM so submitted
geometry becomes visible pixels. Textures/fog/blending are LATER steps — do
not start them, but leave obvious seams for them.

## Where

- Tree: `/Users/hsp/src/claude-code/qemu-master-g3`, branch `g3beige`.
- Build: `ninja -C build-g3 qemu-system-ppc` (must end clean).
- Device: `hw/display/ati_rage128.c` (+ `_int.h`, `_regs.h`, `_2d.c`,
  `_dbg.c`, `hw/display/trace-events`).
- Hook point: the `R128_PM4_OPCODE_3D_RNDR_GEN_PRIM` case in
  `ati_rage128_pm4_parse()` (streamed parser). It already computes the vertex
  stride from VC_FORMAT (`ati_rage128_vc_stride()`), gathers each vertex's
  dwords into `p->p3_vtx[]`, and traces them (`ati_rage128_3d_trace_vert()`).
  Extend it: decode each completed vertex into floats, accumulate per the
  primitive type, emit triangles.
- DO NOT touch the 2D packet cases, the ring parser, or any other tree.
  DO NOT commit — leave the working tree changes for review.
  DO NOT boot a guest OS; the qtest harness below is your test bed.

## Input format (CONFIRMED live — not hypothesis)

`GEN_PRIM` payload: `[VC_FORMAT] [VC_CNTL] [vertices...]`.
Nanosaur: VC_FORMAT `0xa7`, VC_CNTL `0x00030034` = TRI_LIST (type 4), 3
vertices, one triangle per packet. Vertex = 11 little-endian floats:

    x, y, z, rhw, b, g, r, a, fog, s, t

x/y in screen pixels on the render target (0..640/0..480 observed), z in
0..1, colours 0..1. The 8-dword variant (`x,y,z,rhw,b,g,r,a`) also occurs
(untextured/no-fog packets). Field presence follows the `R128_VC_FRMT_*` bits
(see `ati_rage128_vc_stride()` — BGR means three float dwords, A one more).
Handle prim types 4/5/6 (TRI_LIST/FAN/STRIP) — generic accumulation is cheap;
other types may be skipped with the existing unimp-style trace.

Real captured payload for replay: `nanosaur-payload-sample.log.gz` here
(trace lines `... payload[N]=0xXXXXXXXX`; idx 0 starts a packet; packets are
35 payload dwords; prepend the packet3 header `0xC0220x2500` form yourself —
header = `0xC0000000 | ((ndwords-1)<<16) | (0x25<<8)`).

## Render state (registers, all already latched in `s->regs[]`)

Values in brackets are what Nanosaur programs (via packet0 writes) — decode
properly from the register, use the observed values as your test fixture:

- `R128_DST_PITCH_OFFSET_C` (0x1c80) [0x0a0f0300 / 0x0a0fad00 alternating —
  double-buffered]: same encoding as the 2D `DST_PITCH_OFFSET`; reuse/mirror
  the existing 2D context decode (`ati_rage128_2d.c` / the resolve helpers):
  pitch = bits 31:21 in units of 8 pixels (0x50 → 640 px), offset =
  (bits 20:0) << 5 bytes.
- `R128_DP_GUI_MASTER_CNTL_C` (0x1c84) [0x28cc33db]: destination datatype in
  bits 11:8 → 3 = ARGB1555 ("Thousands", 2 bytes/px). Support datatypes 3
  (ARGB1555), 4 (RGB565) and 6 (ARGB8888); anything else: skip the draw with
  a trace (add one, e.g. `ati_rage128_3d_unsupported`).
- `R128_SC_TOP_LEFT_C`/`R128_SC_BOTTOM_RIGHT_C` (0x1c88/0x1c8c)
  [0x0 / 0x01df027f = y..479, x..639]: scissor, inclusive bounds, y in bits
  31:16, x in 15:0. Clip every pixel to it AND to VRAM bounds.
- `R128_Z_OFFSET_C` (0x1c90), `R128_Z_PITCH_C` (0x1c94) [0x00010050 — pitch
  field like dst pitch, low 12 bits, units of 8: 0x50 → 640; ignore the
  bit-16 flag for now], 16-bit Z buffer.
- `R128_Z_STEN_CNTL_C` (0x1c98) [0x00000010]: z test function. Copy the
  `R128_Z_TEST_*` field definitions from the reference
  `/Volumes/Macdata/qemu/doc/rage128-cce/r128_reg.h` (do not include that
  file; copy the needed defines into `ati_rage128_regs.h`). Implement at
  least NEVER/LESS/LEQUAL/ALWAYS; others = nearest sensible + trace once.
- `R128_TEX_CNTL_C` (0x1c9c) [0x193/0x183]: bit 0 = Z_ENABLE, bit 1 =
  Z_WRITE_ENABLE (confirm against the reference r128_reg.h and copy the
  defines). Gate the Z test/write on these.
- `R128_PLANE_3D_MASK_C` (0x1d44) [0xffffffff]: write mask; apply only when
  not all-ones.
- Ignore for this step (leave a short comment naming them): fog regs,
  MISC_3D_STATE_CNTL_REG blending, both texture units, WINDOW_XY_OFFSET
  [always 0 in corpus], stencil.

Convert float colours to the destination format with saturation (there is a
`ati_rage128_vc_col8()` helper already).

## Rasterizer requirements

- Screen-space triangles (the vertices are pre-transformed): standard
  edge-function/barycentric scan over the clipped bbox is fine at this
  resolution. Use the top-left fill rule so shared edges paint exactly once
  (blending arrives in a later step; get the convention right now).
- Interpolate z and b,g,r,a linearly in screen space; write z as 16-bit
  (z*65535 clamped) when z-write is enabled.
- VRAM access: `memory_region_get_ram_ptr(&s->vram)` + explicit bounds
  checks on EVERY address derived from guest-programmed offsets/pitches
  (offsets and pitches are hostile input). Pixel stores little-endian like
  the 2D engine's (mirror `_2d.c`'s conventions).
- After each triangle (or batched per packet), mark the touched rows dirty:
  `memory_region_set_dirty(&s->vram, start, len)` — without this the display
  never repaints.
- Degenerate/zero-area triangles: skip cheaply. NaN/inf floats: reject the
  triangle (guest data is untrusted).
- Add trace event `ati_rage128_3d_tri(x0,y0,x1,y1,x2,y2,argb)` (ints) per
  accepted triangle; keep the existing `_3d_prim`/`_3d_vert` traces working
  unchanged.

## Test bed (deliverable, not optional): qtest harness

Python + the qtest protocol; put it in this directory as `raster_regress.py`.
Pattern:

- Launch: `build-g3/qemu-system-ppc -M g3beige -display none -qtest
  unix:SOCK,server -qtest-log /dev/null -device ati-rage128-pro` (no ROM
  needed for qtest; add `-machine accel=qtest`).
- PCI setup through the Grackle/MPC106 host bridge: config address port
  `0xfec00000`, data port `0xfee00000` (32-bit little-endian writes;
  CF8-style address `0x80000000 | devfn<<8 | reg`, find the device by
  scanning vendor/device 0x1002/0x5245). Program BAR0 (VRAM aperture,
  e.g. 0x84000000), BAR2 (MMIO, e.g. 0x82000000), set the command register
  memory-enable bit.
- Push packets: 32-bit writes to MMIO BAR2 + 0x1000 (any dword in
  0x1000..0x13ff feeds the CCE FIFO parser directly — no mode setup needed).
  First push packet0 writes that program the render state above (a packet0
  header is `(reg>>2) | count-1<<16`-style — read `R128_PM4_PACKET0`-family
  macros in `ati_rage128_regs.h` rather than guessing), then GEN_PRIM
  packets.
- Read back pixels through BAR0 (VRAM aperture 0 is raw little-endian).

Required checks, each printed PASS/FAIL with numbers:
1. **Known-answer triangle**: right triangle (10,10)-(200,10)-(10,150),
   solid colour, ARGB1555 target at offset 0x8000, pitch 640. Assert an
   interior pixel's exact value, a pixel outside the triangle unchanged, and
   the painted-pixel count within ±1% of the analytic area.
2. **Gouraud**: three-colour triangle; assert the three near-vertex pixels
   are near their vertex colours (tolerance ±8 per component).
3. **Z test**: draw far triangle then near triangle overlapping (z-enable
   set via TEX_CNTL_C, LESS): near wins; then reversed order: near still
   wins. Negative control: with Z_ENABLE off, last-drawn wins.
4. **Scissor negative control**: scissor excluding the triangle → zero
   pixels change (prove the instrument can detect: same draw with open
   scissor changes pixels).
5. **Corpus replay**: decompress `nanosaur-payload-sample.log.gz`, rebuild
   the packets (state registers first with the corpus values above), replay
   all of them, assert >0 pixels painted inside 640x480 and no crash, and
   report the painted-pixel count.
6. **2D unharmed**: after all of the above, push one 2D PAINT packet (see
   the `R128_PM4_OPCODE_PAINT` case for the 4-dword form) and assert it
   still fills its rectangle.

## Report back

State what you implemented, every check's measured numbers, anything you
could not verify and why, and any deviation from this contract. A check that
did not run is reported as NOT RUN, never as implied-pass. Do not commit;
do not push; leave the tree building cleanly with your changes in place.
