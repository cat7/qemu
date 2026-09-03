# HANDOFF: OS X Server 1.2v3 (Rhapsody) no longer boots from CD, cannot initialise a disk

Contract for a reader with zero shared context. Read it whole first.
Written 2026-09-03 by the main session from the user's report.

## The report

User, 2026-09-03: "find out why the osx server 12v3 will not boot from cd
and can no longer initialise a hard disk." The wording says REGRESSION:
this machine worked before. Two symptoms, possibly one cause:
1. booting the Server 1.2v3 install CD fails;
2. initialising a hard disk fails.
No screenshot and no error text was supplied, so establishing WHAT the
failure looks like is part of the job. The user has authorised running
their launcher and creating a new 2 GB disk image for the test.

The guest is **Mac OS X Server 1.2v3, i.e. Rhapsody**: XNU on an Old
World Power Mac, not classic Mac OS and not modern OS X. It has its own
quirks, listed under "Rhapsody facts" below.

## Where things are

| what | path |
|---|---|
| QEMU tree, branch `g3beige`, head `c97dc66a95` | `/Users/hsp/src/claude-code/qemu-master-g3` |
| the user's deployed server install (do NOT write into it) | `/Applications/qemu-system-ppc-g3-server12v3/` |
| their launcher (read it; reproduce its options) | `.../qemu-system-ppc-G3-server12v3.command` |
| CURRENT binary, `c97dc66a95`, deployed 12:57 today | `/Applications/qemu-system-ppc-g3-server12v3/qemu-system-ppc` |
| **OLD binary, `aaccaa41d8` from 2026-08-17** (free A/B reference) | `/Applications/qemu-system-ppc-g3-linux/qemu-system-ppc` |
| install CD (read-only, 650 MB) | `/Volumes/Macdata/qemu/iso/Server_1.2v3.iso` |
| the user's installed system, 1 GB (NEVER open read-write) | `/Volumes/Macdata/qemu/hd/Server1.2v3.img` |
| ROM and card FCode ROM | `/Applications/qemu-system-ppc-g3-server12v3/{PowerMacG3v3.ROM,ati_mach_gt.rom}` |
| build dir for new builds | make your own inside a worktree; the user's is `build-g3` (do not clobber) |

Their launcher today (note `-m 512`; it was `-m 1024` earlier today, so the
user has already been changing it, which is itself a clue):

```
-M g3beige -m 512 -bios PowerMacG3v3.ROM -display cocoa
-audiodev coreaudio,id=snd -global awacs.audiodev=snd
-global ati-mach64-gt.romfile=ati_mach_gt.rom
-nic user,model=bmac,mac=00:05:02:12:34:56
-drive file=/Volumes/Macdata/qemu/hd/Server1.2v3.img,format=raw,media=disk,index=0
-drive file=/Volumes/Macdata/qemu/iso/Server_1.2v3.iso,format=raw,media=cdrom,index=2
```

## Ranked suspects (from `git log` since the machine last worked)

All landed after 2026-08-31. Nothing in this list is proven; the point of
the A/B is to stop guessing.

1. **`e78e46d2fe` "g3beige: populate all three DIMM slots so -m beyond
   512MB is real"** (`hw/ppc/mac_oldworld.c`, +44/-18). Changes SPD/DIMM
   presentation. Rhapsody sizes memory from the device tree the ROM builds
   out of SPD. A 512 MB config is exactly the boundary this commit moved.
   **Test `-m 512` AND `-m 1024` on both binaries.**
2. **`a909f3b4f7` "hw/pci-host/grackle: make the MPC106 bank-decode
   registers writable"**. Memory-controller decode: if a guest writes
   these and the model now honours it, RAM can vanish under the OS.
3. **The MESH SCSI series** (`92f4f116e9`, `5fac369859`, `84d4fe9aee`).
   The SCSI controller now really enumerates. The ROM's boot scan probes
   SCSI before ATA (established 2026-09-02: SCSI CD beats ATA CD), so a
   newly-live MESH can change which device the ROM picks or how long it
   spends looking, even with no SCSI drive attached. Directly plausible
   for "will not boot from CD".
4. Governor series (`a9151c44c4`, `6ba4615a67`, `7e02e60fea`,
   `741868ca84`, `329b0333ea`, `35c45e1901`). Rhapsody's clock has a
   history here (see Rhapsody facts).
5. Today's networking commits (`1fb7b2500b`, `a061251ea2`, `eaea9e2921`).
   Lowest suspicion: proven inert on slirp, and this config uses slirp.
   But this binary was deployed at 12:57 TODAY, so it cannot be excluded
   without a test.

## Rhapsody facts you will need (all previously established)

- Rhapsody arms **no VIA T2 one-shots**, so the calibration governor's T2
  window never fires for it. What does fire is the **cpu-probe window**
  (`7e02e60fea`): XNU's `pe_run_clock_test()` loads VIA T1 with 0xFFFF and
  counts 10M CPU clocks. Ungoverned, its clock ran ~3.3x fast.
- Historic standing config was `-plugin libgovernor.dylib,mips=200`
  (commented out in their launcher now) because the STOCK Rhapsody kernel
  overflows 32-bit math in its timebase calibration when the window
  stretches. The in-tree cpu-probe window was supposed to replace it.
  `calibration-governor` defaults to ON at 79 MIPS. **Try
  `-M g3beige,calibration-governor=off` and `=mips=200` as controls**: a
  wrong clock can look exactly like "disk operations fail".
- The ROM is the real Apple `PowerMacG3v3.ROM`. A polluted `nvram.img`
  makes it boot `fd:diags` (flashing floppy) or hang mid-chime; a fresh
  NVRAM is the control. `boot-device=/AAPL,ROM` means "ROM decides", and
  the ROM takes the **lowest-index bootable disk**.
- ATA slots: `index=0` bus0 master, `index=1` bus0 slave, `index=2` bus1
  master, `index=3` bus1 slave. Convention that works: HD at 0, CD at 2.
- The C key at boot selects the first CD but must be **pulsed** (150 ms
  down, 50 ms up); a key held from reset is never seen.

## Method

### Phase 0: reproduce, with controls, WITHOUT touching the user's data

Scratch dir per run under `/Users/hsp/.claude/jobs/886cc763/tmp/`, each
with its own cwd so QEMU makes a fresh `nvram.img`/`pram.img` (a fresh
NVRAM is itself control #1).

- Boot disk: a **qcow2 overlay** whose backing file is
  `/Volumes/Macdata/qemu/hd/Server1.2v3.img` opened READ-ONLY. Never open
  the master read-write; record its mtime before and after every run.
- Initialise target: a NEW 2 GB raw image you create with `qemu-img`
  (the user explicitly authorised this), attached at `index=1`.
- CD: their ISO at `index=2` (read-only by nature).
- Headless: `-display none -qmp unix:<scratch>/qmp.sock,server=on,wait=off`,
  drive it over QMP, take `screendump` PNGs and LOOK at them. That is the
  only way to see where a Rhapsody boot stops.
- Runs are cheap only if bounded: cap each at 6 minutes, `quit` over QMP.

Establish, on the CURRENT binary:
- **T1** CD boot with no HD attached at all. Does the installer appear?
- **T2** CD boot with the overlay HD at index 0 and the blank 2 GB at
  index 1 (their real layout plus the new disk).
- **T3** HD boot (no CD): does the installed system still come up?
- For whichever of these reaches a running system, attempt the disk
  initialisation on the blank 2 GB disk and record exactly what happens.
- If a run needs the C key to pick the CD, pulse it as described.

Capture for every run: the QMP transcript, stderr, and screendumps at
30 s intervals. Name them so a reader can follow.

### Phase 1: is it a regression at all?

Repeat the failing test with the **old binary** (`aaccaa41d8`, 2026-08-17)
from the linux folder. Same scratch method, same options.
- Old passes, current fails -> regression, go to Phase 2.
- Both fail -> not a code regression. Then suspect data or configuration:
  the ISO (verify it is the same file the user always used: `md5`, size
  681920512, dated 2007), the installed image, the NVRAM, or `-m 512`
  versus `-m 1024`. Say so with evidence and stop guessing about commits.
- Both pass -> your harness does not reproduce the user's failure. Report
  that plainly and list precisely what differs from their launcher
  (display backend, audio, cwd, their nvram). Do not invent a cause.

### Phase 2: bisect

`git bisect` between `aaccaa41d8` (good) and `c97dc66a95` (bad) in a
worktree of your own, building `qemu-system-ppc` only, target list
`ppc-softmmu`. Take the configure options from the head of
`/Users/hsp/src/claude-code/qemu-master-g3/build-g3/config.log`. Script
the test so each step is mechanical, and keep the per-step evidence.
Prefer testing the ranked suspects first if a full bisect looks slow:
`e78e46d2fe~1` versus `e78e46d2fe` answers suspect 1 in two builds.

### Phase 3: root cause, then fix

Explain the mechanism in terms of what the guest does and what the model
does, not just "commit X breaks it". Then fix it in the tree, or if the
right answer is a launcher change, say exactly which line and why.
Verify the fix on the failing test AND check you did not break the two
guests that currently work: a Mac OS 9 boot (overlay of
`/Volumes/Macdata/qemu/hd/9.2-G3.img`, desktop screendump) is the
regression control.

## Rules (binding)

- Never open any image under `/Volumes/Macdata/qemu/hd/` read-write.
  Overlays only, master mtime checked before and after.
- Never write into `/Applications/...`; read binaries there, nothing more.
- One guest at a time. The user may run their own QEMU: never `pkill` by
  pattern, kill only pids you started, prefer QMP `quit`.
- Work in your own git worktree off `g3beige`; do not touch the user's
  checkout or its `build-g3`. Commit there; do NOT push, do NOT merge.
  Trailers:
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01WofRUkRsK1ynoHkTKpsk2q`
- Trace logs can reach GB. Bound them, and delete anything over 200 MB.
- A negative result ("it never happens") counts only with a positive
  control proving the instrument could have seen it.

## Report

`doc/server12v3/REPORT-server12v3-regression.md` in your worktree, and a
final message to the main session containing: what the failure actually
looks like (per test, with screendump paths), whether it is a regression,
the bisect result if any, the mechanism, the fix or the recommended
launcher change, the regression control result, and commit hashes. Assume
the reader never saw this file.
