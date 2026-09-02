# g3beige: which device boots when a SCSI CD and an ATA CD are both present

Empirical results on the PowerMacG3v3.ROM (Beige G3), QEMU g3beige machine,
tree `qemu-master-g3` branch `g3beige` with the MESH fixes of 2026-09-02
(uncommitted at the time of writing). Every claim below is one headless
boot (`-display none`) with FRESH `nvram.img`/`pram.img` unless stated,
observed through a `screendump` and the Finder's Get Info "Where:" line of
the boot volume (the top-right icon), which names the bus and ID
("CD-ROM SCSI ID 3 (a)" vs "CD-ROM ATAPI ID 1 (a)").

Configuration used for the two-CD runs:

```
-drive file=81scsi.iso,format=raw,if=none,id=cd0,media=cdrom,read-only=on
-device scsi-cd,drive=cd0,scsi-id=3                 # MESH bus, SCSI ID 3
-drive file=8.1.iso,format=raw,media=cdrom,index=2  # ATA bus 1 master
```

`81scsi.iso` is a byte copy of `8.1.iso` whose HFS volume name in the MDB
(Apple_HFS partition start 964 * 512 + 1024 + 37) was changed from
`Mac OS 8.1` to `SCSI-CD-81` so the Startup Disk panel could tell the two
apart (the Finder desktop still shows both as "Mac OS 8.1" -- it takes the
name from the catalog root, so Get Info's "Where:" is the oracle).

| run | drives | key / NVRAM | booted from | evidence |
|-----|--------|-------------|-------------|----------|
| 5 | scsi-cd ID3 only, no ATA drive | fresh | SCSI CD | run5-t180.png desktop; 1346 selections of target 3, 3033 DMA transfers |
| 6 | scsi-cd ID3 + ATA CD | fresh | **SCSI CD (ID 3)** | run6-info-top.png "CD-ROM SCSI ID 3 (a)"; 2nd volume "CD-ROM ATAPI ID 1 (a)" |
| 4 | scsi-hd ID0 (initialised, no System) + ATA CD | fresh | ATA CD | run4-desk.png; SCSI volume merely mounted |
| 7 | scsi-cd + ATA CD | `c` HELD +1..+45 s | SCSI CD | run7-info-top.png -- but INVALID, see run 8 |
| 8 | 9.2 HD overlay index 0 + ATA CD | `c` HELD +1..+45 s | HD (9.2 desktop) | run8-t170.png -- a held key is NOT seen |
| 8b | 9.2 HD overlay index 0 + ATA CD | `c` PULSED 150/50 ms, 211x | ATA CD (8.1 desktop) | run8b-t150.png -- positive control for the C key |
| 7b | scsi-cd + ATA CD | `c` PULSED, 212x | **SCSI CD (ID 3)** | run7b-info-top.png |
| 9 -> 10 | scsi-cd + ATA CD | Startup Disk: click "Mac OS 8.1" (ATA); quit; relaunch KEEPING nvram | **ATA CD (ATAPI ID 1)** | run9-sd-ata.png, run10-info-top.png |
| 10 -> 12 | scsi-cd + ATA CD | Startup Disk: click "SCSI-CD-81"; quit; relaunch KEEPING nvram | **SCSI CD (ID 3)** | run10-sd-scsi.png, run12-info-top.png |

## Findings

1. **Default order: the MESH (SCSI) CD wins over the ATA CD.** With nothing
   in NVRAM the ROM boots the SCSI CD at ID 3 rather than the ATA CD on
   bus 1 (run 6). The ROM's boot-time scan selects SCSI IDs 0..6 in that
   order (trace: `select fired target=N`), and an initialised-but-empty
   SCSI HD does not divert it (run 4) -- it is "first BOOTABLE volume in
   scan order", SCSI bus first.
2. **The C key selects the first CD in that same order**, i.e. again the
   SCSI CD when both are present (run 7b). The C key does work on this
   ROM/emulator: it moved the boot from the 9.2 HD to the ATA CD (run 8b).
3. **How to press C in QEMU:** a key held down from +1 s is NOT seen
   (run 8 still booted the HD). The ADB keyboard model reports one
   transition and the ROM's ADB init discards it; pulse the key
   (150 ms down / 50 ms up) for the first ~45 s instead (`ckey_pulse.py`).
4. **Startup Disk overrides the default in both directions** across a full
   QEMU quit + relaunch with the same `nvram.img` (runs 10 and 12). The
   panel writes 5 NVRAM bytes the moment an icon is clicked
   (`-trace macio_nvram_write`):

   | selection | 0x1301 | 0x1378 0x1379 0x137a 0x137b |
   |-----------|--------|-----------------------------|
   | ATA CD, bus 1 master | 0x40 | `01 00 20 00` |
   | SCSI CD, ID 3 | 0x40 | `18 00 00 00` |

   `0x18 = 3 << 3` -- SCSI ID in bits 3..5 of byte 0x1378, bus byte 0;
   the ATA encoding matches the one recorded in
   `memory/project_g3_startup_disk_ide_slots.md` (`01 00 20 00` for the
   bus-1 CD). The panel reads the MDB volume name (it showed
   "SCSI-CD-81"), the Finder does not.
5. **Warm restart is a separate open problem (not MESH):** Finder
   Special > Restart / power-key > Restart on a session that had a SCSI
   volume mounted flushed the volume (WRITE(10)s, 2x SYNCHRONIZE CACHE,
   MODE SENSE, REQUEST SENSE, ENARESEL), the machine reset, and the ROM
   then sat forever in Open Firmware's Forth loop at 0xff812b8c
   (`begin ... again` calling `2over`/`<=`/`0branch` with the `ms` delay
   hot at 0xff808e20; MSR 0x2070, translation off), never touching MESH,
   never re-initialising the display, ignoring `mac-boot<CR>` on the ADB
   keyboard. NVRAM 0x1043 (the POST flag) read 0. Not attributed: no
   control run without the SCSI device was done. Cold relaunch is the
   valid persistence test anyway (memory: a soft Restart survives via a
   RAM-resident hint).

## Tooling (scratchpad `mesh/`)

`launch.sh` (env `MESH_TRACE`, `EXTRA_TRACE`, `KEEP_NVRAM`), `drive.py`
(QMP; `goto_abs` closes the loop on the Classic low-mem global `RawMouse`
at 0x82C via `xp /1w 0x82c` -- launch with
`-global ati-mach64-gt.host-cursor-tracking=off` or the mach64
host-cursor handler steals REL motion once the CDM record goes live),
`init_run.py` (Drive Setup Initialize), `bootwatch.py`, `ckey_pulse.py`,
`startupdisk.py`, `parse.py` (reconstructs SCSI transactions from a
`mesh_*` trace).
