# Why an Old World Mac needs help to start Mac OS X

Written 2026-09-06, from measurements on this machine. Every claim below
has a run behind it; the table names them.

## The symptom

Create a fresh g3beige machine whose only disk holds a Mac OS X install,
give it an empty `nvram.img` and `pram.img`, and start it. You get the
flashing floppy with a question mark, forever. The same disk boots
immediately on a machine whose NVRAM has been used before.

## Why

Two things are measured, and one older explanation is withdrawn:

1. **The ROM's own boot scan does not find the Mac OS X volume.** A
   fresh NVRAM has `boot-device` = `/AAPL,ROM`, which means "ROM, you
   decide". With that setting the machine sits at the flashing floppy
   (arm A), and installing the shim below without changing
   `boot-device` does not help (arm F4): whatever the ROM's scan looks
   for, an `nvramrc` does not change it. The description of that scan
   as "a search for a classic System" comes from published
   documentation, not from observing the ROM; what is observed is that it passes a Mac
   OS X volume by and finds a classic one (the 9.2 and 8.5.1 disks boot
   from `/AAPL,ROM`).

2. **An explicit `boot-device` alone is not enough.** Pointed straight
   at the volume (arm D) the boot switches video mode and then dies with
   the CPU at address 0. An `nvramrc` shim is needed as well; what it
   has to contain is the next section.

Withdrawn: the earlier text said "Old World Open Firmware cannot read
HFS+, so it cannot load BootX without a patched `mac-parts`". That came
from web documentation. Arm G1 below boots to the desktop with no
`mac-parts` patch at all, and the disk's HFS wrapper carries no copy of
BootX (its root holds only System, Finder, Desktop DB/DF and a ReadMe),
so how this ROM's OF loads BootX from the wrapped HFS+ volume is not
known from here. It is not, on this evidence, "cannot".

On real hardware you never see this, because you install Mac OS X from
Mac OS 9, and Mac OS 9's Startup Disk control panel writes *both* an
explicit `boot-device` path and an `nvramrc`. Move an OS-X-only disk
into a G3 whose PRAM was never set up, and that machine will not boot
it either. This is also why zapping PRAM on a working Old World OS X
machine breaks it until you re-select the startup disk, and most of
what XPostFacto does.

## What the shim has to do

All arms boot the same 10.0 overlay, headless, with a screendump and a
NIP/MSR sample every 45 s. "Desktop" is the Finder with the Dock;
"NIP 0" is the CPU at address 0 in real mode (MSR 0x40).

| arm | NVRAM | result |
|---|---|---|
| A | fresh default | flashing floppy |
| B | + `ati-mach64-gt.romfile` | flashing floppy — the display ROM is not involved |
| C | + default speed governor | flashing floppy — timing is not involved |
| D | fresh + explicit `ide0/@0:6` | video mode switches, then CPU at NIP 0 |
| F4 | full shim, but `boot-device` left `/AAPL,ROM` | flashing floppy |
| F2 | explicit path + `mac-parts` patches only | NIP 0 |
| F1 | explicit path + memory/vector patches only | NIP 0 |
| F5 | explicit path + both, no `decode-unit` patch | NIP 0 |
| E | the machine's own working NVRAM, path repointed | 10.0 desktop, ~3.5 min |
| F3 | same, rebuilt from a fresh template | 10.0 desktop |
| F6 | explicit path + our own shim, all three blocks | 10.0 desktop, ~90 s |
| F6c | F6 re-run as the control for G1/G2, committed wording (`bootr`) | 10.0 desktop at 90 s |
| **G1** | **F6c minus the `mac-parts` block** | **10.0 desktop at 90 s** |
| **G2** | **F6c minus `qmem`/`qargs` (`: bootr boot ;`)** | **fails: 640x480 black, MSR 0x40, NIP cycling 0x4095xx–0x4097xx** |

Screendumps: `osx10-ab/{F6c,G1,G2}/at-*.png` in the job's tmp folder
(`F6c/at-90.png`, `G1/at-90.png` are the desktop; `G2/at-45.png` and
`at-90.png` are 972-byte black frames).

So two things are jointly required, and that is what the shim contains:

- `mac-io`'s `decode-unit` redefined as `parse-1hex` (F5 fails without
  it; the reading that this is what makes the `@0` in `ide0/@0:6`
  resolve comes from the word's name and published OF documentation,
  not from observing OF);
- the `qmem`/`qargs` half: two low memory ranges released,
  `install-interrupt-vectors` made a no-op, and an empty `machargs` on
  `/chosen` (G2 fails without it; which of the three parts matters was
  not separated).

Not required on this disk, and no longer in the shim: the `mac-parts`
branch patches (G1 boots without them, same timing as with). Only a
10.0 volume was tried; a volume this does not hold for would show up as
arm D's failure, video mode switch then NIP 0, and would be the case to
re-test with the block restored.

The shim in `hw/nvram/mac_nvram.c` is our own Forth doing exactly those
two jobs. Apple's is longer: it patches `mac-parts`, polls the key map
for the boot-time modifier keys and retries the boot thirty times. None
of that is reproduced.

## What QEMU now does

At machine init, `mac_oldworld_pick_startup_device()` looks at the IDE
drives, but **only** when the OF partition is still exactly the default
we stamped (valid, `/AAPL,ROM`, no `nvramrc`). It walks each disk's
Apple partition map, and for every `Apple_HFS*` partition reads the
volume's own header to see what it is blessed for:

- **HFS Plus** (reached through the HFS wrapper when there is one):
  `finderInfo[0]` is the folder the ROM starts from, `finderInfo[3]` a
  Mac OS 8/9 System Folder, `finderInfo[5]` a Mac OS X one (TN1150). The
  volume counts as *classic* when `[3] != 0`, or when `[0] != 0` and
  `[0] != [5]`; it counts as *Mac OS X* when `[5] != 0`. A volume
  blessed only for Mac OS X reads exactly `[0] == [5]` with `[3] == 0`.
- **Plain HFS** (MDB signature `BD`, no `H+` embed): the MDB's
  `drFndrInfo[0]`, at byte 92 (0x5C) of the MDB, is the blessed System
  Folder. Non-zero counts as *classic*; plain HFS never holds Mac OS X.

Why the compound HFS Plus rule and not `[3]` alone: `9.0.4.img` in this
project is a classic-only disk blessed with `[0] = 29, [3] = 0, [5] = 0`.
A `[3]`-only test calls that "no classic system" and, next to a Mac OS X
disk, would have pointed NVRAM at Mac OS X where the ROM would have
booted 9.0.4. Every disk the rule was checked against (2026-09-06):

| disk | volume | `[0]` | `[3]` | `[5]` | classic | OS X |
|---|---|---|---|---|---|---|
| 8.1-G3.img | HFS+ | 24 | 24 | 0 | yes | no |
| 9.0.4.img | HFS+ | 29 | 0 | 0 | yes | no |
| 9.1, 9.2-G3, 9.2-G4, 9.2-pristine, 9.2.1, 9.2.2 | HFS+ | 30 | 30 | 0 | yes | no |
| 10.0.img | HFS+ | 1317 | 0 | 1317 | no | yes |
| 10.1.img | HFS+ | 1633 | 0 | 1633 | no | yes |
| 10.2.img | HFS+ | 2595 | 0 | 2595 | no | yes |
| 10.3.img | HFS+ | 2380 | 0 | 2380 | no | yes |
| 10.4.img | HFS+ | 3321 | 0 | 3321 | no | yes |
| 10.5.img | HFS+ | 149 | 149 | 149 | yes | yes |
| 8.5.1.img | plain HFS | drFndrInfo[0] = 26 | | | yes | no |
| aux/system7.1, 7.5.3, 8.1 | plain HFS | drFndrInfo[0] = 18 | | | yes | no |
| aux/system8.0 | plain HFS | drFndrInfo[0] = 127 | | | yes | no |

The drFndrInfo offset was verified, not taken from the layout table: the
five plain-HFS images above read their blessed ID at 0x5C and zeros at
0x6C, and TN1150 places `drEmbedSigWord` at 0x7C, which is exactly where
a 32-byte `drFndrInfo` starting at 0x5C ends (the 0x6C read on the
wrapped disks lands on `'H+'` and the embed extent, which is why an
earlier read at 0x6C "saw zeros"). Every HFS wrapper on the disks above
reads `drFndrInfo[0] = 2`, its own root: TN1150 says the wrapper carries
"a System file containing the minimum code to locate and mount the
embedded HFS Plus volume", which is what that blessing is for. The
wrapper is therefore looked *through* and never counted itself.

- Any disk carrying a classic system: do nothing. The ROM can boot it,
  and its scan order stays the user's business.
- Otherwise, the first Mac OS X volume found becomes the startup device:
  NVRAM gets `boot-device` = `ideN/@M:P`, the shim as `nvramrc`,
  `boot-command` = `0 bootr `, and `use-nvramrc?` set. It says so on
  stderr when it does this.

Once the guest writes its own NVRAM — which Mac OS 9 will, the first
time anyone touches Startup Disk — the partition is no longer the
default, and none of this runs again.

The partition numbering matches what Apple's own control panel produces:
this machine's saved NVRAM for a 10.2 disk reads `ide1/@1:9`, and our
scan independently picks partition 9 on that disk.

### Limits

- Only the IDE drives are scanned. SCSI (MESH) disks are not looked at.
- The partition map is read assuming 512-byte blocks. A CD image whose
  Apple partition map uses 2048-byte blocks (most `.iso` files in this
  project, e.g. `iso/9.0.4.iso`) is not detected: the reads land on the
  wrong sectors and count as "nothing". Such a CD neither becomes the
  startup device nor counts as a classic system; the ROM's own handling
  of it is unchanged.
- A disk with no driver descriptor block (`ER`) at block 0 — this
  project's `Server1.2v3.img` — is likewise "nothing".

## The NVRAM format, for anyone editing it by hand

Old World OF partition at **0x1800**, length **0x800**:

| offset | meaning |
|---|---|
| 0x00 | signature 0x1275, version 5, page count |
| 0x04 | checksum: `~(ones-complement 16-bit sum with this field zeroed)` |
| 0x06 | `here` — end of the variable table (0x185c) |
| 0x08 | `top` — lowest byte the string heap has reached |
| 0x0c | flags; bit 31-*n* for `little-endian?`, `real-mode?`, `auto-boot?`, `diag-switch?`, `fcode-debug?`, `oem-banner?`, `oem-logo?`, `use-nvramrc?` |
| 0x34 | ten (offset, length) `u16` pairs: boot-device, boot-file, diag-device, diag-file, input-device, output-device, oem-banner, oem-logo, nvramrc, boot-command |
| … | string heap, growing **down** from 0x2000 |

Offsets in the table are absolute NVRAM offsets, not partition-relative.
DingusPPC exposes the same table through its debugger (`printenv` /
`setenv` / `nvedit`) in `devices/common/ofnvram.cpp`; it does not
automate any of this, which is why its users type the boot path in.
