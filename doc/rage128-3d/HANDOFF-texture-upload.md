# HANDOFF: where do Nanosaur's textures live? (rage128 3D, texture fetch)

Contract for a reader with zero shared context. Read SURFACE.md,
HANDOFF-rasterizer.md and HANDOFF-textures-perf.md in this directory first,
then `git log -8 --format=%B` on branch g3beige (the four commits from
2026-09-02 evening: rasterizer, MULTI-packet prefix fix, textures+perf,
"texture offsets are 24-bit").

## The problem

Texture mapping is implemented (`ati_rage128_3d_triangle` + `ati_rage128_tex_*`
in hw/display/ati_rage128_2d.c, harness 32/32) but LIVE, most of Nanosaur's
textures render BLACK. Established tonight, all measured:

- The texture base the model computes from PRIM_TEX_n_OFFSET_C points at VRAM
  that is ALL ZERO at draw time, in every boot, for both readings of the
  register: 30-bit (`raw & 0x3fffffff`, the DRM reading) and 24-bit
  (`raw & 0xffffff`, the current `R128_TEX_OFFSET_MASK` -- committed on the
  strength of ONE boot where 7/8 textures had data at the 24-bit address;
  the NEXT boot had nothing there either, so treat the 24-bit reading as
  UNPROVEN, likely a coincidence of that boot's heap layout). Raw register
  values look like 0xc19a2c00 / 0xc1996600 (bits 31:24 = 0xc1 on the big
  textures), 0x0191d400 (no flags) on small ones, 0x01ff3e00 for an 8x8
  white texture that IS in VRAM at the literal address.
- A whole-VRAM occupancy map during gameplay (one 4KB sample per 256KB) shows
  data only at 0-1.75MB (screen) and in the 27-31.75MB blocks, which are the
  render targets and Z buffers (dumped as images: sky/Z bands). No texture
  heap anywhere in VRAM.
- The PCI GART table (register PCI_GART_PAGE, table at guest RAM 0x01027000
  in the boots seen; 746 entries) maps ONLY card addresses 0x40000-0x329fff --
  the command buffers. Texture card addresses are not mapped, under any
  reading. So textures are not fetched from system memory either.
- Paths that do NOT carry texture data (traced over 45-60s of gameplay
  including a level load): HOSTDATA_BLT (559 blits, all HUD text to dst
  0x8000); the BM_GUI_TABLE bus-master engine (zero descriptors); PM4 unimp
  packets (none); CPU aperture writes (3,056,176 in 60s, ALL aperture 1,
  ALL to 0x20000-0x9c000 = the front buffer); IB dispatch (2781, command
  streams). 2D blits: only frame copies (640x480 screen<->back buffers) and
  small HUD ops.
- So either the upload happens through a path the model silently drops
  (nothing traced reached the texture region), or the data is uploaded and
  then DESTROYED by the model (e.g. a clear/blit/Z-write with a wrong
  offset/pitch/size wiping the texture heap), or the offset encoding is
  something else entirely (relative to a base register? units?). Decide by
  MEASUREMENT: catch the upload.

## Instrument traps (real, cost hours)

- HMP `xp` of the rage128 REGISTER aperture (BAR2, e.g. 0x80804000+reg)
  returns the dword BYTE-SWAPPED (0x00700201 is really 0x01027000). VRAM
  (BAR0 0x84000000+off) and guest RAM reads are fine.
- Render targets and texture addresses are heap-allocated per boot; never
  carry addresses from one boot to the next.
- Type-ahead launch of Nanosaur (`n a n o` + Cmd-O twice) opens a PDF, not
  the app. The folder window opens on the RAGE display, which the default
  `screendump` cannot see. FIX FOR YOU: give the card an id (`-device
  ati-rage128-pro,id=rage,...`) and use `screendump file.ppm device=rage`
  (QMP: {"execute":"screendump","arguments":{"filename":...,"device":"rage"}})
  to SEE the folder, then navigate by keyboard (arrow keys / full-name
  type-ahead) with a screenshot after every step. Verify launch by triangles
  flowing (`ati_rage128_3d_prim` count), not by assumption.

## What to do

1. A VM is ALREADY BOOTING for you (started 2026-09-02 ~22:40): QMP socket
   /tmp/g3nano3.sock, trace output to
   `/private/tmp/claude-502/-Users-hsp-src-claude-code-qemu-master-g3/95e0d9a0-e724-4369-807f-4acdfe15873f/scratchpad/upload.log`
   (events off until you turn them on with HMP `trace-event NAME on`), but it
   has NO device id, so you cannot screendump its rage display. Either use it
   blind (arm traces, then ask... no -- there is no human), or quit it via QMP
   and relaunch with `id=rage`. Launch recipe (the user's config, verbatim):
   ```
   cd <scratchpad>; cp /Applications/qemu-system-ppc-g3-mac-os/{nvram,pram}.img .
   qemu-img create -f qcow2 -b /private/tmp/claude-502/-Users-hsp-src-claude-code-qemu-master-g3/5dc68a6d-c450-4e62-81ba-02e6803ab16b/scratchpad/nano.img -F raw nanoX.qcow2
   /Users/hsp/src/claude-code/qemu-master-g3/build-g3/qemu-system-ppc -M g3beige -m 512 \
     -bios /Applications/qemu-system-ppc-g3-mac-os/PowerMacG3v3.ROM -display sdl \
     -audiodev coreaudio,id=snd -global awacs.audiodev=snd \
     -global ati-mach64-gt.romfile=/Applications/qemu-system-ppc-g3-mac-os/ati_mach_gt.rom \
     -device ati-rage128-pro,id=rage,addr=0x0e,romfile=/Applications/qemu-system-ppc-g3-mac-os/ati_nexus128_103_pci.rom \
     -nic user,model=bmac,mac=00:05:02:12:34:56 \
     -drive file=nanoX.qcow2,format=qcow2,media=disk,index=0 \
     -qmp unix:/tmp/<short>.sock,server,nowait -trace file=<scratchpad>/upload.log
   ```
   Desktop after ~120s (screen hash stabilises; see drive3d.py `waitdesk`).
   The Nanosaur folder is on the desktop ("Nanosaur 1.1.6 f").
2. BEFORE launching the game, arm: ati_rage128_2d_blt, ati_rage128_ctx_write,
   ati_rage128_host_data_reg, ati_rage128_bm_desc, ati_rage128_pm4_unimp,
   ati_rage128_pm4_reg, ati_rage128_3d_tex, ati_rage128_aper_wr (heavy:
   ~50K lines/s -- fine for two minutes, gzip/delete after).
3. Launch Nanosaur (see trap above), get through the menu into the arena
   (space/return), let it run ~20s, disarm, quit via QMP.
4. Analyse: which writes hit VRAM outside the screen (offset > 0x9e000)?
   Where do the texture bases the 3d_tex events name point, and what wrote
   there (or what wrote NEAR there and was later zeroed)? Correlate
   PRIM_TEX_n_OFFSET_C values with the observed uploads to derive the real
   address encoding. Also read `ati_rage128_aper1` handling in
   ati_rage128.c: aperture 1 is the byte-swapped alias in the top 32MB of the
   64MB BAR0 -- check its size/offset mapping is exact (a texture upload
   through aperture 1 at an offset >= 16MB would be a prime suspect if the
   alias were shorter than VRAM).
5. Fix the model (encoding or upload path), prove it with a harness check
   (raster_regress.py, keep 32/32 and add one that reproduces the real
   encoding you found), and with the live game: textures visible. If you can
   screendump device=rage during play, include the screenshot path.

## Rules

Work only in /Users/hsp/src/claude-code/qemu-master-g3 (branch g3beige); no
commits/pushes; do not touch hw/scsi (another agent builds the same tree; if
a build fails in files you did not touch, wait and retry). Scratch under
`<scratchpad>/texup/`; short socket paths under /tmp. Boots cost money: plan
each one; two or three should suffice. Never touch /Volumes/Macdata/qemu/hd/.

## Report

The measured chain: what wrote the texels, where, under what encoding; the
fix; harness numbers; the live result; anything unresolved, plainly.
