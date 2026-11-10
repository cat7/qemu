===========================================
Power Mac G3, G4 and G5 for qemu-system-ppc
===========================================

This repository is a personal fork of QEMU with three PowerPC Macs, each
on its own branch:

- ``G3-rom`` -- the Beige Power Mac G3, on Apple's own ROM.
- ``G4-openbios`` -- a Power Mac G4, on OpenBIOS.
- ``G5-openbios`` -- a Power Mac G5, on OpenBIOS (``qemu-system-ppc64``).

This page is the README of ``G3-rom``, the default branch, and describes
all three. It is an experiment and is not meant for upstream QEMU.

QEMU as a whole is released under the GNU General Public License, version
2. For full licensing details, consult the LICENSE file.

Downloads
=========

Ready-made builds for macOS (Apple silicon and Intel) and 64-bit Windows,
each with a GUI for setting up and starting machines:

=======================  ===============  ==========================================================================
Machine                  Branch           Download
=======================  ===============  ==========================================================================
Beige Power Mac G3       ``G3-rom``       `G3-rom-2026-11-06 <https://github.com/cat7/qemu/releases/tag/G3-rom-2026-11-06>`_
Power Mac G4 (OpenBIOS)  ``G4-openbios``  `G4-openbios-2026-11-05 <https://github.com/cat7/qemu/releases/tag/G4-openbios-2026-11-05>`_
Power Mac G5 (OpenBIOS)  ``G5-openbios``  `G5-openbios-2026-11-05 <https://github.com/cat7/qemu/releases/tag/G5-openbios-2026-11-05>`_
=======================  ===============  ==========================================================================

All releases: https://github.com/cat7/qemu/releases. Two more machines
have their own repositories:

- The Apple Network Server 700/200 (https://github.com/cat7/ANS-rom). It
  runs AIX 4.1.5 on the Network Server ROM 1.1.22, Mac OS 8.5 on the
  Network Server ROM 2.0 with an ATI Rage 128 Pro card, and Windows NT 4.0
  on the Network Server ROM 2.26NT with the NT boot floppy
  (https://github.com/cat7/nt-boot-floppy). macOS and Windows builds with
  a GUI: `ANS-2026-11-10 <https://github.com/cat7/ANS-rom/releases/tag/ANS-2026-11-10>`_.
- The 68k Macintosh II, LC and Quadra models
  (https://github.com/cat7/m68k-rom), with printing to the host's
  printers. macOS build with a GUI:
  `m68k-2026-11-03 <https://github.com/cat7/m68k-rom/releases/tag/m68k-2026-11-03>`_;
  Windows build with a GUI and host printing:
  `m68k-2026-11-10 <https://github.com/cat7/m68k-rom/releases/tag/m68k-2026-11-10>`_.

Building
========

Check out the machine's branch and build only its emulator:

.. code-block:: shell

  git clone -b G3-rom https://github.com/cat7/qemu.git    # or G4-openbios
  cd qemu
  mkdir build
  cd build
  ../configure --target-list=ppc-softmmu --enable-cocoa --enable-slirp
  make qemu-system-ppc

For the G5, clone ``G5-openbios`` and use ``--target-list=ppc64-softmmu``
and ``make qemu-system-ppc64``. ``--enable-cocoa`` is for macOS; on Windows
(MSYS2 UCRT64) use ``--enable-sdl --enable-gtk`` instead.

The G4 and G5 run on OpenBIOS, ``openbios-qemu.elf``. The release zips
have it; to build it yourself, use the branch of the same name of
github.com/cat7/openbios.

Beige Power Mac G3 (``G3-rom``)
===============================

The ``g3beige`` machine of ``qemu-system-ppc``, made to run the real
Beige G3 ROM. It runs Mac OS 8 to 9.2, Mac OS X 10.0 to 10.2, Mac OS X
Server 1.2v3 and Linux.

The machine
-----------

=====================  ======================================================
Model                  Beige Power Mac G3 ("Gossamer")
CPU                    PowerPC 750 (G3), one CPU
Memory                 up to 2047 MB (``-m``); the real board takes 768 MB
Firmware               Apple's Beige G3 ROM (``-bios``)
Host bridge            Grackle
I/O controller         Heathrow: ESCC serial ports, Cuda (ADB, clock, PRAM),
                       SWIM3 floppy, AWACS sound, BMAC Ethernet, MESH SCSI,
                       ATA
Graphics               ATI Mach64 Rage Pro on board; an ATI Rage 128 Pro
                       can be added as a second card
Shared folder          virtio-9p, as a Mac OS volume
=====================  ======================================================

What you need
-------------

- A Beige G3 ROM image, 4 MB. Use the version 3 ROM
  (``PowerMacG3v3.ROM``). It is not included.
- A Mac OS install CD as an image. It is not included.
- An empty disk image::

    qemu-img create -f raw macos.img 4G

Running
-------

.. code-block:: shell

  ./qemu-system-ppc \
    -M g3beige -m 512 \
    -bios PowerMacG3v3.ROM \
    -audiodev coreaudio,id=snd -global awacs.audiodev=snd \
    -nic user,model=bmac \
    -drive file=macos.img,format=raw,media=disk \
    -cdrom install.iso

On Windows use ``-audiodev dsound,id=snd`` and ``-display sdl``.

- **CPU speed.** ``-M g3beige,calibration-governor=on|off|mips=N``. The ROM
  times its CPU-speed constants in a 1 ms window; an emulated CPU that runs
  too fast stores 0 there and Mac OS later quits with "divide by zero"
  errors (the ATAPI CD driver, Open Transport). The governor slows the CPU
  only inside that window. It is on by default (79 MIPS), which classic
  Mac OS needs; Mac OS X, OS X Server and Linux run better with it off.
- **Boot device.** The ROM ignores ``-boot``. It looks for a blessed System
  Folder, preferring the startup disk saved in PRAM. To boot a CD (or
  floppy) for sure, attach only that medium and no hard disk. With both
  attached, pick the CD in the Startup Disk control panel and restart; the
  choice is kept across runs.
- **Disks.** ``-drive file=...,format=raw,media=disk`` is an ATA hard disk;
  repeat it for a second one. ``-cdrom`` is an ATAPI CD-ROM. SCSI disks and
  CDs go on the MESH bus::

    -drive file=disk.img,format=raw,if=none,id=shd0 \
    -device scsi-hd,drive=shd0,scsi-id=0

  The SWIM3 floppy (raw 1440K or 720K images)::

    -drive if=none,id=fd,file=floppy.img,format=raw -global swim3.drive=fd

- **Network.** ``-nic user,model=bmac`` is the onboard BMAC on slirp NAT:
  the guest gets 10.0.2.15 by DHCP, gateway 10.0.2.2. Add
  ``,mac=52:54:00:xx:yy:zz`` to fix the MAC address. ``-nic none``
  removes the card.
- **ATI FCode ROM.** With a real ATI Mach64 Open Firmware FCode ROM as the
  card's expansion ROM, the ROM's Open Firmware runs it and Apple's ATI
  display driver binds, which draws faster than the generic path::

    -global ati-mach64-gt.romfile=ati_fcode.rom

  It is tested with the Mach64 3D Rage "GT" FCode ROM (ATI p/n
  109-39200-00, FCode 1.53). It is not included. With the driver bound,
  the card's DDC reports a "Multiple Scan Display"; set its resolution with
  ``-global ati-mach64-gt.xres=1024 -global ati-mach64-gt.yres=768``.
- **Second card.** ``-device ati-rage128-pro,addr=0x0e,romfile=rage128pro.rom``.
- **Shared folder** (Mac OS 8.6 and later). The card needs the FCode ROM
  ``virtio-9p.rom``, built from github.com/cat7/classicvirtio:

  .. code-block:: shell

    -fsdev local,id=hostshare,path=/path/to/folder,security_model=none,macos_compat=on \
    -device virtio-9p-pci,fsdev=hostshare,mount_tag=Shared,romfile=virtio-9p.rom,addr=0x0d

- **PRAM and NVRAM.** ``pram.img`` (Cuda PRAM: startup disk, network
  settings, time zone) and ``nvram.img`` (Open Firmware NVRAM) are kept in
  the current directory, created if missing. Delete both to zap the PRAM.
  Give each guest OS its own directory: Mac OS X rewrites the Open
  Firmware settings, and a classic Mac OS boot from the same files then
  hangs halfway through the startup chime.
- **Cursor.** Under classic Mac OS the machine moves the ATI hardware
  cursor to the host pointer itself, because the guest stops doing so
  during boot. Clicks still go through ADB.

Known limits
------------

- One CPU.
- Short system sounds under classic Mac OS can have audible glitches; the
  Sound Manager's buffer is only a few milliseconds deep.
- The Mach64 2D engine has no 1 and 4 bpp modes, polygon or trapezoid
  fills, or rotating VRAM patterns.

Power Mac G4 (``G4-openbios``)
==============================

The ``mac99`` machine of ``qemu-system-ppc``, extended into a New World
Power Mac G4 that runs on OpenBIOS, so no Apple ROM is needed. It runs
Mac OS 9.2 and Mac OS X 10.0 to 10.5.

The machine
-----------

=====================  ======================================================
Model                  PowerMac3,1 (PowerMac3,6 with more than 4 CPUs)
CPU                    PowerPC 7400 (G4); ``-smp`` up to 32
Memory                 up to 2 GB (``-m``)
Firmware               OpenBIOS (``openbios-qemu.elf``)
Host bridge            UniNorth: PCI, AGP and internal PCI buses
I/O controller         KeyLargo: OpenPIC, ESCC serial ports, PMU (or Cuda),
                       two ATA buses, Screamer sound, NVRAM
Ethernet               Sun GEM (``sungem``)
USB                    OHCI; NEC uPD720101 USB 2.0 card (``nec-usb=on``)
Graphics               QEMU standard VGA by default; ATI Rage 128 Pro or
                       ATI Radeon 9800 (card ROM needed)
Shared folder          virtio-9p, as a Mac OS volume
=====================  ======================================================

What you need
-------------

- ``openbios-qemu.elf`` (see Building).
- A Mac OS 9.2 or Mac OS X 10.0 to 10.5 install CD or DVD as an image
  (``.iso``, ``.toast``, ``.cdr``, ``.dmg``). It is not included.
- An empty disk image::

    qemu-img create -f raw macosx.img 20G

- For an ATI card: its Mac ROM image. It is not included.

Machine options
---------------

``-M mac99,option=value,...``

- ``via=pmu|pmu-adb|cuda`` -- the power controller. Use ``pmu``: more than
  one CPU needs a PMU. ``pmu`` gives a USB keyboard and mouse, ``pmu-adb``
  and ``cuda`` ADB ones.
- ``nec-usb=on`` -- adds the NEC USB 2.0 card (Mac OS X 10.3 and later),
  for passing host USB devices through.
- ``decr-cap-ms=N`` -- fires each CPU's decrementer at least every N ms,
  so idle CPUs do not sleep too long.

Installing
----------

The NVRAM is kept only in an attached file. Make it once:

.. code-block:: shell

  dd if=/dev/zero of=nvram.img bs=8192 count=1

Boot the install CD with ``-boot d``:

.. code-block:: shell

  ./qemu-system-ppc \
    -M mac99,via=pmu -m 1024 \
    -bios openbios-qemu.elf \
    -audiodev coreaudio,id=snd -global screamer.audiodev=snd \
    -nic user,model=sungem \
    -drive file=macosx.img,format=raw,media=disk \
    -cdrom install.iso \
    -drive if=none,id=nvr,file=nvram.img,format=raw \
    -global macio-nvram.drive=nvr \
    -boot d

Erase the disk with Disk Utility (Drive Setup on Mac OS 9) and install as
on a real Mac.

Running
-------

Leave out ``-cdrom`` and boot the disk with ``-boot c``. Add ``-smp 2``
or more for more CPUs. On Windows use ``-audiodev dsound,id=snd`` and
``-display sdl``.

- **Graphics cards.** ``-vga none`` and one of:

  - ``-device ati-rage128-pro,romfile=rage128pro.rom`` -- for Mac OS 9.
  - ``-device ati-radeon9800,gl=off|on|fast,romfile=radeon9800.rom`` --
    Mac OS X 10.3 to 10.5 only, with Quartz Extreme and OpenGL. ``gl=off``
    draws in software, ``on`` through host OpenGL (exact), ``fast`` through
    host OpenGL (fastest). On macOS, ``gl-api=metal`` uses Metal instead.

- **Shared folder.**

  .. code-block:: shell

    -fsdev local,id=hostshare,path=/path/to/folder,security_model=none,macos_compat=on \
    -device virtio-9p-pci,fsdev=hostshare,mount_tag=Shared

  Mac OS X 10.4 and 10.5 need the osx9p kext (github.com/cat7/osx9p).
  Mac OS 8.6 to 9.2 need the driver that ``ndrvloader`` (from
  github.com/cat7/classicvirtio) loads before the boot::

    -device loader,addr=0x4000000,file=ndrvloader \
    -prom-env 'boot-command=init-program go'

- **USB tablet.** ``-device usb-tablet`` makes the mouse follow the host
  pointer in Mac OS X.

Known limits
------------

- At most 2 GB of memory.
- Two of KeyLargo's three ATA buses are modelled.
- The Radeon 9800 runs only on Mac OS X 10.3 to 10.5. Mac OS 9's
  shared-folder driver does not fit in OpenBIOS beside the Radeon 9800's
  ROM; use the Rage 128 Pro there.
- Mac OS X 10.5 deadlocks above about 16 CPUs.

Power Mac G5 (``G5-openbios``)
==============================

The 64-bit ``mac99`` machine of ``qemu-system-ppc64``, turned into a Power
Mac G5 (PowerMac7,3) that runs on OpenBIOS, so no Apple ROM is needed. It
runs Mac OS X 10.3.9 to 10.5.

The machine
-----------

=====================  ======================================================
Model                  PowerMac7,3
CPU                    PowerPC 970FX (G5); ``-smp`` up to 32
Memory                 above 2 GB continues at 4 GB, as on the real machine
Firmware               OpenBIOS (``openbios-qemu.elf``)
Host bridge            U3: AGP bus, HyperTransport, DART (IOMMU), MPIC
I/O controller         K2: MPIC, ESCC serial ports, PMU, sound (I2S with
                       a TAS3004 codec), two OHCI USB controllers, NVRAM
Storage                K2 ATA-100 (optical drives), K2 SATA ports A and B
                       (hard disks)
Ethernet               K2 GMAC (``sungem``)
USB 2.0                NEC uPD720101 card (EHCI)
Graphics               QEMU standard VGA by default (1024 x 768); ATI
                       Radeon 9800 or ATI Radeon 7000 in the AGP slot
                       (card ROM needed)
Shared folder          virtio-9p, as a Mac OS volume
=====================  ======================================================

What you need
-------------

- ``openbios-qemu.elf`` (see Building).
- A Mac OS X 10.3.9 (G5 build), 10.4 or 10.5 install DVD as an image
  (``.iso``, ``.toast``, ``.cdr``, ``.dmg``). It is not included.
- An empty disk image::

    qemu-img create -f raw macosx.img 40G

- For an ATI card: its Mac ROM image. It is not included.

Machine options
---------------

``-M mac99,via=pmu -cpu 970fx``. The keyboard and mouse are USB.

- ``decr-cap-ms=N`` -- fires each CPU's decrementer at least every N ms,
  so idle CPUs do not sleep too long.

The NVRAM (two 8 KB flash banks) is kept in ``nvram.img`` in the current
directory, created if missing.

Installing
----------

Hard disks go on SATA, the DVD on the ATA-100 bus. Boot the DVD with
``-boot d``:

.. code-block:: shell

  ./qemu-system-ppc64 \
    -M mac99,via=pmu -cpu 970fx -smp 2 -m 2048 \
    -bios openbios-qemu.elf \
    -audiodev coreaudio,id=snd0 -global macio-newworld.audiodev=snd0 \
    -nic user,model=sungem \
    -drive file=macosx.img,format=raw,if=none,id=sata0 \
    -device ide-hd,bus=sata.0,drive=sata0 \
    -cdrom install.iso \
    -boot d

Erase the disk with Disk Utility and install as on a real Mac.

Running
-------

Leave out ``-cdrom`` and boot the disk with ``-boot c``. On Windows use
``-audiodev dsound,id=snd0`` and ``-display sdl``.

- **Graphics cards.** ``-vga none`` and one of:

  - ``-device ati-radeon9800,bus=pci.0,addr=0x10,gl=off|on|fast,romfile=radeon9800.rom``
    -- Quartz Extreme and OpenGL. ``gl=off`` draws in software, ``on``
    through host OpenGL (exact), ``fast`` through host OpenGL (fastest).
  - ``-device ati-vga,model=rv100,bus=pci.0,addr=0x10,agp=on,romfile=radeon7000.rom``
    -- the Radeon 7000.

- **Shared folder.** The card goes in a PCI-X slot:

  .. code-block:: shell

    -fsdev local,id=hostshare,path=/path/to/folder,security_model=none,macos_compat=on \
    -device virtio-9p-pci,bus=pcix1,addr=0x2,fsdev=hostshare,mount_tag=Shared

  Mac OS X needs the osx9p kext (github.com/cat7/osx9p).

- **USB tablet.** ``-device usb-tablet`` makes the mouse follow the host
  pointer.

Known limits
------------

- Mac OS X 10.3 uses at most 2 CPUs.
- Mac OS X 10.5 deadlocks above about 16 CPUs.
