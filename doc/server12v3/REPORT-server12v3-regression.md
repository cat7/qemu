# Mac OS X Server 1.2v3 (Rhapsody): why the install CD would not boot

Investigation 2026-09-03. Brief: `HANDOFF-server12v3-regression.md` (same
directory). Worktree branch `server12v3-regression`, off `g3beige`
`c97dc66a95`.

## Answer in one paragraph

It is **not a regression from any recent commit**. It is a decade-old
generic QEMU IDE bug that only bites a guest which checks the ATA `DSC`
status bit, and the classic Mac OS ROM's ATA Manager is such a guest.
QEMU completes the ATA power-management commands (STANDBY IMMEDIATE and
friends) with status `0x40` instead of `0x50`, clearing `DSC`. The Mac OS
disk driver then polls Alternate Status forever. The Server 1.2v3 install
CD hits this the moment it has to **mount a volume on the ATA hard
disk** — which is exactly what "initialise a hard disk" creates. One
line-per-entry fix in `hw/ide/core.c` clears it; commit `b35a737e96`.

## What the failure actually looks like

All runs headless, driven over QMP, screendumps every 30-60 s.
Scratch root: `/Users/hsp/.claude/jobs/886cc763/tmp/runs/`.

| run | config (current binary `c97dc66a95`) | result |
|---|---|---|
| T1 | CD only, no disks | **Boots**: full CD Mac OS desktop within 30 s (`T1/shot-030.png`) |
| T2c | CD + blank 2 GB at index 1 | **Boots** (`T2c/shot-final.png`) |
| T2f | CD + blank 1 GB at index 0 | **Boots** (`T2f/shot-final.png`) |
| T2 / T2d / T2e | CD + the user's `Server1.2v3.img` (qcow2 overlay) at index 0 | **Hangs** (`T2e/shot-final.png`) |
| T6c | CD + a Mac OS 9.2 HFS+ disk (`9.2-G3` copy, boot blocks zeroed so the ROM skips it) | **Hangs**, identically (`T6c/shot-final.png`) |
| T4-nodrv | CD + `Server1.2v3.img` copy with both `Apple_Driver_ATA` partitions retyped `Apple_Free` | **Hangs** |
| T4-nohfs | CD + `Server1.2v3.img` copy with the `Apple_HFS` partition retyped `Apple_Free` | **Boots** (`T4-nohfs/sm-final.png`) |

The hang has a very specific look, and it is **not** a grey screen, a
panic, or an Open Firmware prompt: the CD's Mac OS gets all the way
through extension loading, paints its desktop pattern and the menu-bar
clock, and then freezes there — no Apple menu, no Finder menus, no disk
icons, no Trash, and the clock stops ticking. Every screendump from 60 s
onwards is byte-identical.

Isolation (T4) shows the trigger is the **HFS partition**, not the
disk's driver partitions. T6c shows it is **not** the user's image being
damaged: an ordinary Mac OS 9.2 HFS+ volume hangs the CD in exactly the
same way.

## Mechanism

Traced run `T5-trace` / `T7-trace` (`-trace enable=ide_*,dbdma_*`). The
last thing that happens on the hard-disk bus is:

```
wr @0x6 Device/Head = 0xE0        ; LBA mode, drive 0
wr @0x2 Sector Count = 0x02
wr @0x3/4/5 LBA = 0x0004C0        ; = sector 1216 = Apple_HFS partition start
wr @0x7 Command  = 0xC8           ; READ DMA
dbdma channel 0xb runs, latency 290 us, completes
rd @0x0 Alt Status = 0x50         ; DRDY|DSC -- fine
wr @0x6 Device/Head = 0xA0
wr @0x7 Command  = 0xE0           ; STANDBY IMMEDIATE
rd @0x0 Alt Status = 0x40         ; DRDY, DSC now CLEAR
rd @0x0 Alt Status = 0x40         ; ... 15 million more times
```

The guest then loops forever: one `Device/Head` write followed by 201
Alternate Status reads, repeating. `info registers` at the hang shows the
CPU pinned at NIP `0x003d71fc`, LR `0x003d71f8`, `MSR=0xd072`
(user mode, interrupts enabled) — a tight guest-side poll, not a wedged
device and not a stalled DMA.

In `hw/ide/core.c` the ATA power-management commands are `cmd_nop`
entries flagged `HD_CFA_OK` with no `SET_DSC`:

```c
[WIN_STANDBYNOW1]  = { cmd_nop, HD_CFA_OK },   /* 0xE0 */
[WIN_IDLEIMMEDIATE]= { cmd_nop, HD_CFA_OK },
...
```

and `ide_bus_exec_cmd()` completes them as

```c
s->status = READY_STAT | BUSY_STAT;
...
s->status &= ~BUSY_STAT;                       /* -> 0x40 */
if ((ide_cmd_table[val].flags & SET_DSC) && !s->error)
    s->status |= SEEK_STAT;                    /* skipped */
```

so the command actively clears the `DSC` bit that the preceding READ DMA
had left set. Real drives report `0x50` here, and every neighbouring
entry in the same table already does: `WIN_CHECKPOWERMODE1/2`,
`WIN_SETFEATURES`, `WIN_SEEK`, `WIN_VERIFY*`, `WIN_RECAL`, and the
asynchronous `ide_flush_cb()` all set `SEEK_STAT`. The power-management
no-ops are the odd ones out.

`git log -L` dates `[WIN_STANDBYNOW1] = { cmd_nop, ALL_OK }` to
`d9033e1d3a` (2015), and `SET_DSC` itself to the 2013 `ide_cmd_table`
conversion. So the defect predates every commit on the suspect list.

### Why this reads to the user as "cannot initialise a disk"

With no mountable volume anywhere on the ATA bus, the CD boots fine
(T1/T2c/T2f) — the Mac OS never runs the mount-then-standby sequence.
The moment a disk carries a mountable HFS/HFS+ volume, the Finder mounts
it during startup, the driver spins the disk down, and the machine
hangs. Initialising a disk is precisely what creates that volume, so the
first successful initialise made every subsequent CD boot hang. That is
the loop the user was in.

## The fix

`b35a737e96` — `hw/ide: the power-management no-ops must leave DSC set`.
Adds `| SET_DSC` to the ten `cmd_nop` power-management entries
(STANDBY IMMEDIATE, IDLE IMMEDIATE, STANDBY, IDLE, SLEEP; both opcode
aliases each). 10 lines, `hw/ide/core.c` only.

### Verification (binary built from this worktree)

| run | config | result |
|---|---|---|
| F1 | Server CD + Mac OS 9.2 HFS+ disk | **Boots to the CD desktop, both volumes mounted** (`F1-os9hfs/sm-final.png`) |
| F2 | Server CD + the user's `Server1.2v3.img` overlay at index 0 + blank 2 GB at index 1 (their exact layout) | **Boots to the CD desktop, "untitled" mounted** (`F2-userimg/sm-final.png`) |
| R1 | Mac OS 9.2 boot from `9.2-G3.img` overlay, no CD — regression control | **Full desktop** (`R1-os9/sm-final.png`) |

`meson test --suite qtest-ppc`: 15 ok, 0 fail, 2 skipped, 1 timeout
(`prom-env-test`). The timeout was A/B'd against a rebuild of the same
worktree with the patch stashed and **hangs identically without the
patch** — its `/ppc/prom-env/g3beige` subtest is pre-existing breakage on
this tree, unrelated.

## Other findings, recorded for the user

1. **`/Volumes/Macdata/qemu/hd/Server1.2v3.img` contains no operating
   system.** Its partition map is a normal Apple map ending in a 1 GB
   `Apple_HFS` partition whose embedded HFS+ volume is named "untitled",
   `lastMountedVersion` `8.10`, ~8 MB of 1 GB used. It is a
   freshly-initialised empty disk (mtime 2026-09-03 05:01), which is why
   it cannot boot. The real Rhapsody install is the *other* file,
   `Server1.2v3-patched.img` (2 GB, February): `MOSX_OF3_Booter`
   `Apple_Boot` + `SecondaryLoader` `Apple_Loader` +
   `Apple_Rhapsody_UFS` "Mac OS X Server 1.2".
2. **The C key does not select the CD** in this setup. Twenty synthetic
   pulses (150 ms down / 350 ms up, from t=1 s) never diverted the ROM
   from a bootable hard disk (`T6b`). Worse, a *fast* C pulse train
   (50 ms gaps) wedged an otherwise healthy guest at "Starting Up…"
   (`T1c`, `T6b`) — synthetic key floods are their own hazard, and the
   two early "hangs" in T1c/T6b are that, not the bug under
   investigation. To boot the CD, remove or unbless the hard disk, or
   use the Startup Disk control panel.
3. `Server1.2v3-patched.img` booted with a **fresh** NVRAM gives the
   flashing-`?` floppy (`B1-rhapsody/sm-shot-120.png`): the ROM finds
   nothing bootable without the `boot-device` the user's own
   `nvram.img` carries. Not investigated further; out of scope.
4. The old 2026-08-17 binary (`aaccaa41d8`,
   `/Applications/qemu-system-ppc-g3-linux/qemu-system-ppc`) also fails
   the same test, differently: a "divide by zero" system error during
   Mac OS 9 "Starting Up…" (`T2d-old/sm-final.png`). Both binaries fail,
   so there was never a code regression to bisect; the current build is
   the better of the two, reaching the menu bar before it stalls.

## Safety

`/Volumes/Macdata/qemu/hd/` was opened read-only throughout: qcow2
overlays with the master as backing file, or `qemu-img convert` / `cp`
copies in scratch. `Server1.2v3.img` mtime `1788404484` and
`9.2-G3.img` mtime `1788206875` were unchanged before and after every
run. Nothing was written into `/Applications`. One guest at a time; only
pids started by this session were killed.
