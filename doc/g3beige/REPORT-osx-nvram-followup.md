# REPORT: tightening the Old World "fresh NVRAM boots Mac OS X" change

Follow-up to `e722ad7862`, written 2026-09-06 against the contract
`HANDOFF-osx-nvram-followup.md`. Worktree
`/Users/hsp/.claude/jobs/886cc763/wt-osx-nvram-followup`, branch
`osx-nvram-followup` off `g3beige`. Not pushed, not merged.

Scratch files (NVRAM images, scripts, screendumps, logs) are under
`/Users/hsp/.claude/jobs/886cc763/tmp/osx10-ab/`; paths below are
relative to that folder unless absolute.

## 1. Shim necessity: the two missing arms

All three arms boot a fresh copy of the same 10.0 overlay
(`10.0-overlay.qcow2` on `/Volumes/Macdata/qemu/hd/10.0.img`, read
only) headless on the deployed `/Applications/.../qemu-system-ppc`, with
the NVRAM supplied whole (so QEMU's own default-partition logic does not
run), a screendump and a NIP/MSR sample every 45 s for 6 steps.
NVRAMs were built by `mkg.py` (a copy of `mkf6.py`) with the committed
wording: word `bootr`, boot-command `0 bootr `, boot-device `ide0/@0:6`.

| arm | nvramrc | t=45 s | t=90 s | t=135..270 s | verdict |
|---|---|---|---|---|---|
| F6c (control) | full three-block shim, 459 bytes | NIP 0x9b39c MSR 0x49030, progress bar (`F6c/at-45.png`, 34 KB) | 10.0 desktop (`F6c/at-90.png`, 870 KB) | desktop | positive control valid |
| G1 | F6c minus the `dev /packages/mac-parts` ... `qE` block, 315 bytes | kernel running, "Initializing network" (`G1/at-45.png`) | 10.0 desktop (`G1/at-90.png`, 870 KB) | desktop | **mac-parts block not needed** |
| G2 | F6c minus `qmem`/`qargs` (`: bootr boot ;`), 204 bytes | NIP 0x409704 MSR 0x40, black 640x480 (`G2/at-45.png`, 972 B) | NIP 0x409574 MSR 0x40, black (`G2/at-90.png`) | same, NIP cycling 0x4095xx-0x4097xx | **memory/machargs half needed** |

Log: `g.log`. Screendumps were looked at, not just sized: F6c and G1
at 90 s are the Finder desktop with the Dock and the "OSX 10.0" volume
icon; G2 is an all-black frame at the ROM's initial 640x480.

Combined with the earlier arms (F1 memory-only: NIP 0; F2 mac-parts
only: NIP 0; F5 memory + mac-parts, no decode-unit: NIP 0; F6 all
three: desktop), the necessity picture is:

- `decode-unit` patch: necessary (F5).
- `qmem`/`qargs`: necessary (G2). Which of its three parts matters was
  not separated.
- `mac-parts` patches: **not necessary on this disk** (G1). Dropped
  from `oldworld_osx_boot_shim[]`, together with the `qL` (BLpatch)
  helper that only it used. The shim is now 5 definitions + the mac-io
  patch, and is exactly G1's text minus the unused `: qL BLpatch ;`.

Mechanism, as far as it is known: the 10.0 disk's HFS wrapper root holds
only `System`, `Finder`, `Desktop DB`, `Desktop DF` and
`Where_have_all_my_files_gone?` (read from the wrapper catalog; no file
of type `tbxi`), so OF is not loading a wrapper copy of BootX. How this
ROM's OF loads BootX off the wrapped HFS+ volume without the patch was
not observed. The old claim "Old World OF cannot read HFS+" is withdrawn
in the code comment and the doc. Only a 10.0 volume was tried; 10.1 and
10.2 disks exist in the project and were **not** booted (an extra run
outside the contract's list). If a volume ever needs the block back,
the signature would be arm D's: video-mode switch, then NIP 0.

## 2. Plain HFS: the MDB offset, verified

The contract's layout puts `drFndrInfo` at MDB byte 92 (0x5C). Checked
three ways, all read-only:

1. Five plain-HFS images (MDB `BD`, no `H+` embed) read a non-zero first
   word at 0x5C and all zeros at 0x6C:

   | image | drFndrInfo[0] @0x5C | words @0x6C |
   |---|---|---|
   | `/Volumes/Macdata/qemu/hd/8.5.1.img` part 5 | 26 | 0,0,0,0 |
   | `hd/aux/system7.1.img` part 3 | 18 | 0,0,0,0 |
   | `hd/aux/system7.5.3.img` part 3 | 18 | 0,0,0,0 |
   | `hd/aux/system8.0.img` part 1 | 127 | 0,0,0,0 |
   | `hd/aux/system8.1.img` part 5 | 18 | 0,0,0,0 |

2. On the wrapped disks (8.1-G3, 9.2-G3, 10.0, 9.0.4, 9.1 and every
   OS X CD) the 32-bit read at 0x6C+16 = 0x7C returns 0x482B0005, i.e.
   `'H+'` followed by the embed extent start -- so 0x6C is 16 bytes
   into `drFndrInfo`, and the "all zeros at 0x6C" of the earlier session
   was `drFndrInfo[4..7]`, not `[0..3]`.

3. TN1150's HFS Wrapper section places `drEmbedSigWord` (formerly
   `drVCSize`) at offset 0x7C; a 32-byte `drFndrInfo` that ends there
   starts at 0x5C. The surrounding fields also line up on every image
   read: `drVN` length byte at 36, `drFilCnt` at 84 equals `drNmFls` at
   12 (5 files in each wrapper root), `drDirCnt` at 88.

`8.1-G3.img`, named in the contract as a plain-HFS candidate, is an HFS+
wrapped volume (`H+` embed, `[0]=[3]=24`), so it could not serve; the
`aux/` images and `8.5.1.img` did.

Every HFS wrapper reads `drFndrInfo[0] = 2` (its own root). TN1150:
"When creating the wrapper, Mac OS includes a System file containing
the minimum code to locate and mount the embedded HFS Plus volume and
continue booting from its System file" -- the root blessing is for that
stub. The code only reads `drFndrInfo` on a volume with no `H+` embed,
so a wrapper never counts as classic by itself.

## 3. The new detection rule

`mac_oldworld_volume_systems()` in `hw/ppc/mac_oldworld.c`:

- plain HFS (`BD`, no `H+` embed): `has_classic |= drFndrInfo[0] != 0`
  (MDB byte 92). Never Mac OS X.
- HFS Plus (directly, or through the wrapper): with `finderInfo[0]`,
  `[3]`, `[5]` read at volume-header bytes 80, 92, 100:
  `has_classic |= [3] != 0 || ([0] != 0 && [0] != [5])`;
  `has_osx |= [5] != 0`.

Checked with `rule-check.py` (a host-side mirror of the C logic) on
every relevant image, read-only:

| image | volume | [0] | [3] | [5] | classic | OS X |
|---|---|---|---|---|---|---|
| 8.1-G3.img | HFS+ | 24 | 24 | 0 | yes | no |
| **9.0.4.img** | HFS+ | **29** | **0** | 0 | **yes** | no |
| 9.1, 9.2-G3, 9.2-G4, 9.2-pristine, 9.2.1, 9.2.2 | HFS+ | 30 | 30 | 0 | yes | no |
| 10.0.img | HFS+ | 1317 | 0 | 1317 | no | yes |
| 10.1.img | HFS+ | 1633 | 0 | 1633 | no | yes |
| 10.2.img | HFS+ | 2595 | 0 | 2595 | no | yes |
| 10.3.img | HFS+ | 2380 | 0 | 2380 | no | yes |
| 10.4.img | HFS+ | 3321 | 0 | 3321 | no | yes |
| 10.5.img | HFS+ | 149 | 149 | 149 | yes | yes |
| 10.1/10.2/10.3 second (data) volumes | HFS+ | 0 | 0 | 0 | no | no |
| 8.5.1.img | plain HFS | 26 | | | yes | no |
| aux/system7.1, 7.5.3, 8.1 | plain HFS | 18 | | | yes | no |
| aux/system8.0 | plain HFS | 127 | | | yes | no |
| Server1.2v3.img | no `ER` block 0 | | | | nothing | nothing |
| iso/9.0.4.iso (2048-byte map, plain HFS `[0]=17 [3]=17 [2]=2`) | | | | | nothing | nothing |

`9.0.4.img` is the case the contract predicted: a classic-only disk
blessed only in `[0]`. Under the old `[3]`-only rule it read as "no
classic", and next to a Mac OS X disk QEMU would have pointed NVRAM at
Mac OS X.

## 4. Limits, stated

In the comment above `mac_oldworld_pick_startup_device()` and in the doc:
SCSI (MESH) disks are not scanned; the partition map is read with a
512-byte block assumption, so a CD image whose Apple partition map uses
2048-byte blocks is not detected (reads land on the wrong sectors and
count as "nothing"); a disk with no `ER` descriptor is likewise
nothing. Nothing was implemented for either.

## 5. Wording changes to the necessity claims

`hw/nvram/mac_nvram.c`, comment above `pmac_oldworld_nvram_set_osx_startup()`:

- was: "The ROM's own "/AAPL,ROM" boot scan only ever finds a classic
  Mac OS system, and Old World Open Firmware cannot read HFS+ at all
  [...] an nvramrc that patches OF's mac-parts package so it can load
  BootX, patches mac-io's decode-unit so the unit address in that path
  parses, and releases the low memory BootX needs. Both halves are
  required -- either one alone gets as far as switching the video mode
  and then dies with the CPU at address 0 (measured, 2026-09-06)."
- now: "with the default boot-device "/AAPL,ROM" the ROM's own boot scan
  finds nothing (measured: a fresh NVRAM, and a fresh NVRAM plus a
  working nvramrc with boot-device left at /AAPL,ROM, both sit at the
  flashing floppy), and an explicit boot-device alone gets as far as the
  video-mode switch and then dies with the CPU at address 0. [...] its
  Startup Disk control panel writes an explicit boot-device AND an
  nvramrc, and that nvramrc is what this shim stands in for."

Comment above `oldworld_osx_boot_shim[]`:

- was: "Three jobs, and every one of them was proven necessary by
  dropping it and watching the boot die"; "mac-parts: Old World OF
  cannot read HFS+, so its partition package is branch-patched until it
  can load BootX"; "mac-io decode-unit: makes the "@0" unit address
  [...] parse as hex. Without it the path does not resolve".
- now: "Two jobs, each shown necessary by removing it from the working
  shim and watching the boot fail on a Mac OS X 10.0 volume (2026-09-06,
  with the full shim booting to the desktop on the same binary as the
  control)"; decode-unit: "Without it (arm F5) the boot reaches the
  video-mode switch and ends with the CPU at address 0. The reading that
  this is what lets the "@0" [...] resolve comes from the word's name
  and from published OF documentation, not from watching OF"; qmem/qargs:
  "Without them (arm G2, ": bootr boot ;") the screen never leaves
  640x480 black and the CPU idles in real mode (MSR 0x40) around
  0x4095xx. Which of the three parts matters, and why, was not
  separated"; plus a "Not in the shim" paragraph: "Removing that block
  from our working shim (arm G1) still boots the 10.0 volume to the
  desktop in ~90 s [...] the earlier claim that "Old World OF cannot
  read HFS+" came from web documentation and is not supported by this
  measurement. Only 10.0 was tried."

`doc/g3beige/oldworld-osx-boot.md`: the "Why" section's reason 2 ("Old
World Open Firmware cannot read HFS+ [...] its mac-parts package
predates HFS+") is replaced by a measured reason 2 ("An explicit
boot-device alone is not enough", arm D) and a "Withdrawn" paragraph;
"So three things are jointly required" becomes "So two things are
jointly required", with the F6c/G1/G2 rows added to the arm table; the
detection section is rewritten for the new rule with the evidence
table; a Limits subsection is added; `boot-command` = `bootosx` is
corrected to `0 bootr `.

## 6. Verification on the worktree binary

`build/qemu-system-ppc` from this worktree, final shim (the `mac-parts`
block removed), the plain-HFS and compound HFS+ rules in. Scripts
`verify3.sh` / `verify3b.sh` (copies of `verify.sh` / `verify2.sh` with
`WT=` pointed here, a ps guard, and the new arm). All disks are qcow2
overlays on read-only masters; master mtimes before and after:
`10.0.img` 2026-09-06 01:35:20, `9.2-G3.img` 2026-09-05 22:15:13,
`8.5.1.img` 2026-08-31 10:18:46 -- unchanged. Every screendump named
was looked at.

| arm | disks | stderr | NVRAM after | screen | dump |
|---|---|---|---|---|---|
| osx-alone | 10.0 @ index 0 | "NVRAM: no disk this ROM can start on its own, but ide0/@0:6 holds Mac OS X -- pointing a fresh NVRAM at it" | `ide0/@0:6`, `0 bootr `, nvramrc 300 bytes, valid | 10.0 desktop at 90 s | `V3-osx-alone/at-90.png` |
| os9-alone | 9.2-G3 @ 0 | none | `/AAPL,ROM`, `boot`, no nvramrc | 9.2 desktop | `V3-os9-alone/at-270.png` |
| both-disks | 9.2-G3 @ 0, 10.0 @ 1 | none | untouched | 9.2 desktop, "OSX 10.0" volume mounted | `V3-both-disks/at-270.png` |
| **osx-plus-hfs** (new) | 10.0 @ 0, 8.5.1 (plain HFS) @ 1 | none | untouched | 8.5.1 desktop ("untitled" window, "OSX 10.0" mounted) from 45 s | `V3-osx-plus-hfs/at-45.png`, `at-270.png` |
| first boot (verify3b) | 10.0 @ 0, fresh NVRAM | the NVRAM message | `ide0/@0:6`, `0 bootr `, 300 bytes | 10.0 desktop at 90 s | `V3-second/first-90.png` |
| second boot, same NVRAM | same disk, NVRAM as the guest left it | none ("no NVRAM message -- correct on a second boot") | unchanged | 10.0 desktop at 90 s | `V3-second/second-90.png` |

Ten guest boots in all this session (F6c, G1, G2, four verify3 arms,
two verify3b boots), one at a time, each preceded by a check that no
`qemu-system` process was running; the user's own QEMU was never seen.

## 7. qtest-ppc

`make check-qtest-ppc` in the worktree build: 15 OK, 0 fail, 2 skipped
(cdrom-test, qos-test), 1 timeout (prom-env-test, 360 s) -- the same
result `e722ad7862` recorded before this change. Log:
`build/qtest-ppc.log`.

## 8. Commits

Branch `osx-nvram-followup` in the worktree above, two commits on top of
`e722ad7862`, not pushed, not merged:

1. `593194be0e` -- plain-HFS volumes count as classic via drFndrInfo[0];
   HFS+ classic test becomes `[3] != 0 || ([0] != 0 && [0] != [5])`;
   SCSI / 2048-byte-CD limits stated (`hw/ppc/mac_oldworld.c`, the doc's
   detection section).
2. HEAD -- the `mac-parts` block dropped from the shim; necessity
   claims rewritten to the arms (`hw/nvram/mac_nvram.c`, the doc's Why
   and shim sections, this report).
