# Rage 128 (Pro) 3D surface — the checklist the software rasterizer is built against

Status (2026-09-02 evening): **Gouraud + Z + scissor triangle engine landed**
(`ati_rage128_3d_triangle()`, harness-proven), GEN_PRIM wire format confirmed
live, and the per-frame clear/present packets decode correctly (see "Why the
scene was black"). Textures (perspective-correct, all combine modes, alpha test/blend) landed
the same evening with a 3.1x per-pixel-overhead win (`raster_bench.py`); fog
(FOG_ENABLE is set on every Nanosaur triangle) is the next functional gap. This document enumerates
the 3D register/packet surface the rasterizer consumes, cross-referenced
against the live Nanosaur corpus.

Design decision (settled, do not relitigate): **software rasterizer writing
directly into emulated VRAM**, no host GL. Rationale in the ledger — the R350
GL backend's upload/readback/fence tax made it slower than software 2D; RAVE-era
workloads (<=832x624x16, ~10-30 Mpix/s fill) are trivially within CPU reach.

Sources: Rage 128 Pro RRG (`doc/rage128-cce/rage128-RRG-G04500-C-text.txt`,
section 2.1.6), Linux DRM `r128_reg.h` (`doc/rage128-cce/r128_reg.h`), the CCE
PM4 addendum (`doc/rage128-cce/RagePro128-cce-pm4-addendum.md`).

## Corpus status (2026-09-02, second pass — VERTEX LAYOUT CONFIRMED)

**Payload capture done; the GEN_PRIM wire format is now ground truth, not
hypothesis.** Dual-card config (onboard mach64 + rage128 at addr 0x0e with the
retail nexus ROM); the 9.2 guest image's Monitors setting makes the rage the
primary display — that, not a rage-only config, is how the card goes active.
Live Nanosaur gameplay, 72,364 GEN_PRIM packets with full payload
(`nanosaur-payload-sample.log.gz` here; decoded stream in the session
scratchpad as `nanosaur-3d-decoded.log.gz`):

- Every packet: `vc_format=0xa7`, `vc_cntl=0x00030034` (TRI_LIST, walk 3,
  num=3) — **one triangle per packet**, 35 payload dwords = 2 + 3x11.
- Payload = `[VC_FORMAT] [VC_CNTL] [vertex x3]` (the Linux DRM GEN_INDX
  layout minus the buffer dwords, as predicted).
- **Vertex (11 dwords, every component its own little-endian float — this is
  the CCE FPU path, so the BGR format bits mean three dwords, not a packed
  colour):**
  `x, y, z, rhw, b, g, r, a, fog, s, t`
  with x/y in screen pixels (0..640/0..480), z 0..1 (~0.9997 observed),
  rhw ~1.0, colours 0..1, s/t 0..1. Verified by decoding raw payload bytes
  on exactly those boundaries — all fields sane simultaneously.
- The morning sample's count=26 shape (990 packets) is then
  2 + 3x8 = xyz + rhw + b,g,r,a: untextured, no fog.
- `PM4_VC_FPU_SETUP` = 0x415e: 3D mode, Gouraud, no culling, CW front,
  OGL flat-shade convention.
- State around the triangles: double-buffered 640x480x16 (two alternating
  DST_PITCH_OFFSET_C/Z_OFFSET_C pairs, pitch 640), Z_PITCH_C 0x00010050,
  one scissor (SC_*_C 0,0..639,479), TEX_CNTL_C rewritten per triangle
  (0x193 dominant), SEC_TEX_CNTL_C live, single MISC_3D_STATE_CNTL_REG
  value 0x00510200.

The device now parses GEN_PRIM/GEN_INDX_PRIM (streamed parser): stride from
VC_FORMAT, per-vertex decode traced as `ati_rage128_3d_prim`/`_3d_vert`/
`_3d_indx_prim` — trace-only, no rasterizer yet. Audio side note from the same
session: wavcapture of Nanosaur gameplay shows healthy 44.1k stereo signal with
whole seconds of exact digital silence interleaved — possible awacs pacing/
underrun (the mac99 tumbler class); needs the user's ear + a dedicated pass.

Capture recipe for when the card is active (proven tooling this session):
- trace events: `ati_rage128_pm4_p3_hdr` (every packet3 opcode+count),
  `ati_rage128_pm4_unimp` (packets the model skips — the frontend's to-do list),
  `ati_rage128_pm4_reg` (packet0/1 register writes, name-decoded).
- Use runtime `trace-event ati_rage128_* on/off` windows around the workload,
  NEVER boot-long file traces (a full-boot mesh trace once hit 756 MB).
- End-of-run inventory: `qom-get /machine/peripheral-anon/device[N] silent-regs`.

## Assets (found this session, all under /Volumes/Macdata/qemu)

- **Installed RAVE stack** in the 9.2.2 image (`hd/9.2.2.img`, read via a scratch
  copy): `System Folder/Extensions/` has `ATI Rage 128 3D Accelerator`, `Classic
  RAVE`, `Apple QD3D HW Driver`, `Apple QD3D HW Plug-In`, and the QuickDraw 3D
  core (`QuickDraw 3D`, `QuickDraw 3D RAVE`, `QuickDraw 3D IR`, `QuickDraw 3D
  Viewer`). Nothing to install — the guest is corpus-ready once the card is active.
- **Workload app**: `Applications (Mac OS 9)/Graphing Calculator` (does
  QD3D/RAVE 3D surface plots — a keyboard-drivable 3D workload).
- **QuickDraw 3D installers** on the 8.5.1 and 8.6 ISOs
  (`:Software Installers:QuickDraw 3D:Install QuickDraw 3D`) if a clean install is
  wanted; `QuickDraw 3D Viewer` + a 3DMF file is the minimal RAVE exerciser.
- **ATI retail driver set**: `ati-downloads/ui42_payload/ATI Universal Installer 4/`
  (`ATI Rage 128 3D Accelerator`, `ATI Nexus Driver`, etc.) — matched-set drivers
  for the retail PCI ROM this device advertises.


## Why the scene was black with a working rasterizer (2026-09-02, later)

Live: Nanosaur showed HUD pieces but the moving 3D scene stayed black. Not
the rasterizer -- the 2D parser. Each frame the game clears the Z buffer
(colour 0xffffffff) and the colour back buffer (sky 0x7bd87bd8) with
PAINT_MULTI whose GMC (0x12f033da) has DST_PITCH_OFFSET_CNTL + DST_CLIPPING
set: `[GMC][DST_PO][SC_TOP_LEFT][SC_BOTTOM_RIGHT][colour][rects]`. The parser
knew only the pitch/offset dword, took SC_TOP_LEFT (0) as the colour and the
scissor as a rectangle, so Z was cleared to 0 every frame and every terrain
triangle (z ~0.99, LESS) was rejected; the HUD draws with Z off. Presentation
is a BITBLT_MULTI back->front whose GMC (0x52cc33ff) has bits 0-3 all set:
`[GMC][SRC_PO][DST_PO][SRC_SC_BOTTOM_RIGHT][SC_TOP_LEFT][SC_BOTTOM_RIGHT]
[rects]` -- the three swallowed dwords decoded byte for byte to the source
clip (361,510) and the window's screen rectangle (9,118)-(369,627). General
rule now in both parsers (`ati_rage128_gmc_prefix_reg()`): after the
context come SRC_PO (bit 0), DST_PO (bit 1), SRC_SC_BOTTOM_RIGHT (bit 2),
SC_TOP_LEFT + SC_BOTTOM_RIGHT (bit 3), then the opcode payload. Render
targets are heap-allocated per boot (this run: 510x361 window, dst
0x1b48000 pitch 512, Z 0x1ae8000), so never hard-code the corpus offsets.

## CCE packet3 opcodes (header 0xC000xx00, OPCODE = (h>>8) & 0xff)

Drawing packets the model already handles (2D): NOP 0x10, PAINT 0x11/CNTL 0x91,
BITBLT 0x12/0x92, HOSTDATA_BLT 0x14/0x94, SCALING 0x16/0x96, TRANS_SCALING
0x17/0x97, PAINT_MULTI 0x9A, BITBLT_MULTI 0x9B, POLYSCANLINES 0x18/0x98.

**3D packets — NONE handled yet (the frontend's core work):**
| opcode | name | meaning |
|---|---|---|
| 0x20 | `3D_SAVE_CONTEXT`   | save the 3D pipeline context block |
| 0x21 | `3D_PLAY_CONTEXT`   | restore/replay a saved context |
| 0x23 | `3D_RNDR_GEN_INDX_PRIM` | render indexed primitive (vertex array + indices) |
| 0x25 | `3D_RNDR_GEN_PRIM`  | render primitive, vertices inline in the packet |

`3D_RNDR_GEN_PRIM` (0x25) is the workhorse; its exact wire format is now
CONFIRMED from live payload bytes — see "Corpus status" above. GEN_PRIM and
GEN_INDX_PRIM are parsed and traced by the device (`ati_rage128_3d_*` events);
0x20/0x21 (save/play context) remain unimplemented-skip.

## 3D register groups (RRG 2.1.6) with the key offsets

**Setup Engine** — draw/color/texture setup:
- `SETUP_CNTL` 0x1bc4, `WINDOW_XY_OFFSET` 0x1bcc, `SCALE_3D_CNTL` 0x1a00
  (mode: NOOP/SCALE/TEXMAP_SHADE bits 6-7; alpha-test func bits 24-26),
  `SCALE_3D_DATATYPE` 0x1a20.

**Scissor / window**:
- `SC_LEFT` 0x1640, `SC_RIGHT` 0x1644, `SC_TOP` 0x1648, `SC_BOTTOM` 0x164c,
  `SC_TOP_LEFT_C` 0x1c88, `SC_BOTTOM_RIGHT_C` 0x1c8c,
  `DEFAULT_SC_BOTTOM_RIGHT` 0x16e8 (max 0x1fff x/y).

**Specular / Color / Z / Alpha interpolators**:
- `Z_STEN_CNTL_C` 0x1c98 (z-buffer + stencil),
  `MISC_3D_STATE_CNTL_REG` 0x1ca0, `PLANE_3D_MASK_C` 0x1d44,
  `CONSTANT_COLOR_C` 0x1d34, `FOG_COLOR_C` 0x1cac,
  `FOG_3D_TABLE_START/END/DENSITY` 0x1810/0x1814/0x181c,
  `DESTINATION_3D_CLR_CMP_VAL/MSK` 0x1820/0x1824,
  `CLR_CMP_CLR_3D` 0x1a24, `CLR_CMP_MASK_3D` 0x1a28.

**Texture Mapping**:
- `TEX_CNTL_C` 0x1c9c (SEC_TEXMAP_ENABLE bit 5, ALPHA_TEST_ENABLE bit 10),
  `PRIM_TEX_CNTL_C` 0x1cb0, `PRIM_TEXTURE_COMBINE_CNTL_C` 0x1cb4,
  secondary-texture pitch/size/height shift fields (16/20/24).
- S/T sample offsets to mipmap starts + quadratic interpolator config
  (RRG "Texture Mapping registers"; offsets to be pinned from a textured
  0x25 capture — Graphing Calculator surfaces are Gouraud, likely untextured,
  so a textured game is needed to exercise this group).

**GMC 3D enable**: `GMC_3D_FCN_EN` (bit 27 of the GMC dword in a drawing
packet's context) selects the 3D function path vs plain 2D.

**Concurrent Command Engine (RRG 2.1.7)** — vertex walker + FPU:
- `PM4_VC_FPU_SETUP` 0x071c, `COMPOSITE_SHADOW_ID` 0x...  (a count of 3D
  primitives executed — useful as a model self-check).

## Frontend build order (updated 2026-09-02)

1. ~~Activate the card~~ — DONE: no config change needed; the user's 9.2 image
   already selects the rage as primary monitor (Monitors control panel).
2. ~~Capture a GEN_PRIM payload / decode the vertex layout~~ — DONE, layout
   confirmed (see Corpus status). Parser + decode traces are in the device.
3. ~~Implement Gouraud triangle rasterization~~ — DONE offline 2026-09-02
   (`ati_rage128_3d_triangle()` in `_2d.c`; contract in
   `HANDOFF-rasterizer.md`, proof harness `raster_regress.py` — 6/6 checks:
   exact known-answer pixel counts, Gouraud corners, Z both orders + z-off
   control, scissor negative control, corpus replay, 2D unharmed). A live
   Nanosaur boot (does terrain actually appear?) is still OWED.
4. Single-texture mapping: TEX_CNTL_C (0x193/0x183 observed) + PRIM_TEX_CNTL_C
   + TEX_SIZE_PITCH_C + PRIM_TEX_n_OFFSET_C mip chain, s/t from the vertex.
4b. ~~Single-texture mapping~~ — DONE 2026-09-02 (harness 32/32: nearest +
   bilinear exact texels, orientation proven, perspective proven vs affine,
   565/4444/8888 formats, alpha test + blend; see HANDOFF-textures-perf.md
   and the agent findings recorded in ati_rage128_regs.h: TEXMAP_ENABLE =
   TEX_CNTL_C bit 4, base-level offset slot = TEX_SIZE index, MIP_MAP_DISABLE
   always set by the RAVE driver).
5. Fog (per-vertex fog float + FOG_COLOR_C; FOG_ENABLE set on every corpus
   triangle), secondary texture (SEC_TEXMAP_ENABLE never set in the corpus),
   as the picture demands.
