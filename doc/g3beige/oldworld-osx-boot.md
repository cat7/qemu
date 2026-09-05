# Why an Old World Mac needs help to start Mac OS X

Written 2026-09-06, from measurements on this machine. Every claim below
has a run behind it; the table names them.

## The symptom

Create a fresh g3beige machine whose only disk holds a Mac OS X install,
give it an empty `nvram.img` and `pram.img`, and start it. You get the
flashing floppy with a question mark, forever. The same disk boots
immediately on a machine whose NVRAM has been used before.

## Why

Two independent reasons, and both have to be fixed for a boot:

1. **The ROM's own boot scan only finds classic Mac OS.** A fresh NVRAM
   has `boot-device` = `/AAPL,ROM`, which means "ROM, you decide", and
   what the ROM decides with is a search for a bootable classic System.
   A Mac OS X volume is blessed in `finderInfo[5]`, with nothing classic
   on it, so the scan passes it by. Installing the shim below does *not*
   change this: the scan is in ROM and never consults `nvramrc` (arm F4).

2. **Old World Open Firmware cannot read HFS+.** Even pointed straight
   at the volume it cannot load BootX, because its `mac-parts` package
   predates HFS+.

On real hardware you never see this, because you install Mac OS X from
Mac OS 9, and Mac OS 9's Startup Disk control panel writes *both* an
explicit `boot-device` path and an `nvramrc` that patches Open Firmware
until it can read the volume. Move an OS-X-only disk into a G3 whose
PRAM was never set up, and that machine will not boot it either. This is
also why zapping PRAM on a working Old World OS X machine breaks it
until you re-select the startup disk, and most of what XPostFacto does.

## What the shim has to do

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
| **F6** | **explicit path + our own shim** | **10.0 desktop, ~90 s** |

So three things are jointly required, and any one missing lands in the
same place:

- `mac-parts` branch-patched so OF can load BootX from HFS+;
- `mac-io`'s `decode-unit` patched to parse hex, or the `@0` in a path
  like `ide0/@0:6` does not resolve;
- the low memory BootX loads into released, OF stopped from reinstalling
  its interrupt vectors over it, and an empty `machargs` on `/chosen`.

The shim in `hw/nvram/mac_nvram.c` is our own Forth doing exactly those
three jobs. Apple's is longer: it also polls the key map for the
boot-time modifier keys and retries the boot thirty times. None of that
is reproduced.

## What QEMU now does

At machine init, `mac_oldworld_pick_startup_device()` looks at the IDE
drives, but **only** when the OF partition is still exactly the default
we stamped (valid, `/AAPL,ROM`, no `nvramrc`). It walks each disk's
Apple partition map, and for every `Apple_HFS*` partition reads the
volume header — hopping through the HFS wrapper when there is one — to
see what it is blessed for: `finderInfo[3]` is a Mac OS 8/9 System
Folder, `finderInfo[5]` a Mac OS X one.

- Any disk carrying a classic system: do nothing. The ROM can boot it,
  and its scan order stays the user's business.
- Otherwise, the first Mac OS X volume found becomes the startup device:
  NVRAM gets `boot-device` = `ideN/@M:P`, the shim as `nvramrc`,
  `boot-command` = `bootosx`, and `use-nvramrc?` set. It says so on
  stderr when it does this.

Once the guest writes its own NVRAM — which Mac OS 9 will, the first
time anyone touches Startup Disk — the partition is no longer the
default, and none of this runs again.

The partition numbering matches what Apple's own control panel produces:
this machine's saved NVRAM for a 10.2 disk reads `ide1/@1:9`, and our
scan independently picks partition 9 on that disk.

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
