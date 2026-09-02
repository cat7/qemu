# HANDOFF: g3beige MESH SCSI -- make it work for real

Contract for a reader with zero shared context. Read this whole file, then
the two commit messages it names, before touching anything.

## State you inherit (verified facts, 2026-09-02)

- `hw/scsi/mesh.c` in `/Users/hsp/src/claude-code/qemu-master-g3` (branch
  `g3beige`) models the Beige G3's MESH SCSI controller. Two commits carry
  the design and the evidence -- read both messages in full
  (`git log --format=%B -1 <hash>`):
  - `92f4f116e9` -- bus-phase model, deferred IRQs, PIO FIFO (the WIP
    groundwork; documents every divergence from the real chip that was
    found, and the trace method).
  - `5fac369859` -- transaction engine completed: SEQ_BUS_FREE as dispatch
    probe, status/COMMAND COMPLETE latched by SEQ_STATUS/SEQ_MSG_IN, PIO
    Data Out drained continuously. RESULT: the ROM's boot-time SCSI scan
    completes and Drive Setup 1.4 (booted from the 8.1 CD) enumerates a
    `scsi-hd` and arms Initialize.
- **USER-REPORTED, 2026-09-02, verified real:** clicking **Initialize** in
  Drive Setup **hangs Mac OS**. The commit itself says Initialize/mount and
  the DMA path were never exercised -- this is that gap.
- **USER-REPORTED:** booting from a `scsi-cd` on the MESH bus was never
  tested and the user could not get it to work.
- The reference that has a CONFIRMED-WORKING MESH: DingusPPC at
  `/Users/hsp/src/dingusppc` (`devices/common/scsi/mesh.cpp`,
  `scsibusctrl.cpp`, `scsihd.cpp`, `scsicdrom.cpp`). Rules: `git status` it
  first and never leave instrumentation in it (a stray debug edit once faked
  a "hang"); its ATAPI DMA is a known PIO-fallback stub -- do NOT mirror
  that as a design; for DMA the model to follow is this tree's own DBDMA
  usage (`hw/misc/macio/mac_dbdma.c`, and how `hw/ide/macio.c` drives it).
- Prior session's raw MESH dialogues (register/phase/IRQ traces of the real
  .MESH driver + the 8.1 ROM scan) are in
  `/private/tmp/claude-502/-Users-hsp-src-claude-code-qemu-master-g3/5dc68a6d-c450-4e62-81ba-02e6803ab16b/scratchpad/`
  as `mesh-dialogue.txt`, `mesh-dialogue2.txt`, `mesh-wedge.txt`,
  `mesh-slow.log`. Same directory: `ds.py` (QMP helper used to drive Drive
  Setup by keyboard), `venv/bin/python`, `scsiblank.img` (1 GB raw blank).

## Goals, in order

1. **Initialize works.** Root-cause the hang when Drive Setup initializes
   the `scsi-hd` and fix it. Expect the untested write/DMA path (WRITE(6)/
   WRITE(10) of partition map + HFS structures, possibly via the DBDMA
   channel rather than PIO; also MODE SENSE/READ CAPACITY/verify passes).
   Acceptance: Initialize completes, the volume MOUNTS on the desktop, a
   file can be copied to it, and the disk is still mountable after a
   guest restart (data persisted through the block layer).
2. **scsi-cd on MESH works and BOOTS.** `-device scsi-cd,drive=cd0,scsi-id=3`
   with the 8.1 ISO: the ROM's boot scan must find it and boot Mac OS 8.1
   from it with NO ATA CD present. Then also with an ATA CD present.
3. **Answer, with evidence: how does the machine pick the boot device when
   a SCSI CD and an ATA CD are both present?** Mac OS does not choose --
   the ROM/Open Firmware does, from the PRAM/NVRAM startup-device
   selection (Startup Disk control panel) with a scan-order fallback and
   the C-key override. Read
   `/Users/hsp/.claude/projects/-Users-hsp-src-claude-code-qemu-master-g3/memory/project_g3_startup_disk_ide_slots.md`
   first: it documents this machine's boot-device/nvramrc mechanism and
   four NVRAM edits that are dead ends. Establish the actual order
   empirically on this ROM (which device boots by default; what C does;
   how Startup Disk selects a SCSI device) and write it down.

## How to test (verbatim recipe that produced the enumeration result)

```
S=<your scratch dir>
cd $S && rm -f pram.img nvram.img            # CLEAN NVRAM/PRAM EVERY RUN
qemu-img create -q -f raw $S/scsiblank.img 1G  # fresh blank target each run
/Users/hsp/src/claude-code/qemu-master-g3/build-g3/qemu-system-ppc \
  -M g3beige -m 512 -bios /Volumes/Macdata/qemu/rom/PowerMacG3v3.ROM \
  -display none -audiodev none,id=snd -global awacs.audiodev=snd \
  -global ati-mach64-gt.romfile=/Applications/qemu-system-ppc-g3-mac-os/ati_mach_gt.rom \
  -nic none \
  -drive file=$S/scsiblank.img,format=raw,if=none,id=sd0 \
  -device "scsi-hd,drive=sd0,scsi-id=0,vendor=QUANTUM,product=FIREBALL ST4.3S,ver=0F0C" \
  -drive file=/Volumes/Macdata/qemu/iso/8.1.iso,format=raw,media=cdrom,index=2 \
  -trace 'enable=mesh_*' -trace file=$S/mesh.log \
  -qmp unix:/tmp/<short>.sock,server,nowait
```
- The QUANTUM identity is REQUIRED: Drive Setup's mechanism check lists the
  stock "QEMU HARDDISK" as `<not supported>`.
- Boot to the 8.1 desktop takes ~110 s. Then type-ahead: `u t i l` + Cmd-O
  opens the CD's Utilities folder, `d r i v` + Cmd-O opens Drive Setup;
  `ds.py` shows the QMP key/chord helpers and `screendump`. Drive the
  Initialize dialog the same way (arrow/tab/return) and SCREENSHOT every
  state -- a screenshot is your only view; do not assume a click landed.
- A "hang" must be characterised, not just observed: is the CPU spinning
  on a MESH register poll (trace shows repeated reads), waiting for an
  interrupt that never comes (last IRQ in the trace), or wedged in DBDMA
  (channel status)? The commit messages show how the prior session read
  these traces.
- Boots cost real money and time: plan each run to answer a specific
  question, cap each at ~6 minutes, and always `quit` via QMP.
- Never touch `/Volumes/Macdata/qemu/hd/*.img` directly; boot ISOs read-only
  are fine; anything writable goes through a fresh qcow2 overlay or a
  scratch raw image you created.
- Trace files: MESH traces are small; never enable broad `*` traces for a
  whole boot; delete or gzip logs when done.

## Constraints

- Work only in this tree; do not touch other trees or /Applications.
- Do not commit or push; leave the tree building cleanly
  (`ninja -C build-g3 qemu-system-ppc`) with changes in place.
- Keep the established principles: no synchronous interrupt raised inside
  the guest's own register store (deferred via timer), phase model as the
  single source of truth, every divergence from the real chip justified by
  DingusPPC behaviour or a live trace -- not by "it made the guest happy".
- Regression: after your changes the ATA-CD boot of 8.1 still reaches the
  desktop, and Drive Setup still enumerates the scsi-hd (the result you
  inherit must not be lost).

## Report back

Per goal: status (DONE / PARTIAL / NOT RUN with reason), the mechanism you
found (what the guest did, what the model did wrong), the fix, and the
evidence (screenshot paths, trace excerpts, exact commands). State plainly
what remains broken. Numbers and observations, not adjectives.
