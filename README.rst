================================================
Power Mac G5 with OpenBIOS for qemu-system-ppc64
================================================

This ``G5-openbios`` branch is a personal fork of QEMU. It turns the 64-bit
``mac99`` machine of ``qemu-system-ppc64`` into a Power Mac G5
(PowerMac7,3) that runs on OpenBIOS, free firmware, so no Apple ROM is
needed. It runs Mac OS X 10.3.9 to 10.5 with several CPUs, more than 2 GB
of memory, SATA, sound, USB 2.0, ATI Radeon graphics with OpenGL and a
shared host folder. It is an experiment and is not meant for upstream
QEMU.

QEMU as a whole is released under the GNU General Public License, version
2. For full licensing details, consult the LICENSE file.

Download
========

Ready-made builds for macOS (Apple silicon and Intel) and 64-bit Windows,
with a GUI for setting up and starting machines:

  https://github.com/cat7/qemu/releases/tag/G5-openbios-2026-11-05

All releases: https://github.com/cat7/qemu/releases

The machine
===========

=====================  ======================================================
Model                  PowerMac7,3
CPU                    PowerPC 970FX (G5); ``-smp`` up to 32
Memory                 above 2 GB continues at 4 GB, as on the real machine
Firmware               OpenBIOS, ``G5-openbios`` branch of
                       github.com/cat7/openbios (``openbios-qemu.elf``)
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
=============

- ``openbios-qemu.elf``, the firmware. The release zip has it; to build it
  yourself, use the ``G5-openbios`` branch of github.com/cat7/openbios.
- A Mac OS X 10.3.9 (G5 build), 10.4 or 10.5 install DVD as an image
  (``.iso``, ``.toast``, ``.cdr``, ``.dmg``). It is not included.
- An empty disk image::

    qemu-img create -f raw macosx.img 40G

- For an ATI card: its Mac ROM image. It is not included.

Building
========

.. code-block:: shell

  mkdir build
  cd build
  ../configure --target-list=ppc64-softmmu --enable-cocoa --enable-slirp
  make qemu-system-ppc64

Only ``qemu-system-ppc64`` is needed. ``--enable-cocoa`` is for macOS; on
Windows (MSYS2 UCRT64) use ``--enable-sdl --enable-gtk`` instead.

Machine options
===============

``-M mac99,via=pmu -cpu 970fx``. The keyboard and mouse are USB.

- ``decr-cap-ms=N`` -- fires each CPU's decrementer at least every N ms,
  so idle CPUs do not sleep too long.

The NVRAM (two 8 KB flash banks) is kept in ``nvram.img`` in the current
directory, created if missing.

Installing
==========

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
=======

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
============

- Mac OS X 10.3 uses at most 2 CPUs.
- Mac OS X 10.5 deadlocks above about 16 CPUs.
