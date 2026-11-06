==============================================
Power Mac G4 with OpenBIOS for qemu-system-ppc
==============================================

This ``G4-openbios`` branch is a personal fork of QEMU. It extends the
``mac99`` machine of ``qemu-system-ppc`` into a New World Power Mac G4
that runs on OpenBIOS, free firmware, so no Apple ROM is needed. It runs
Mac OS 9.2 and Mac OS X 10.0 to 10.5, with several CPUs, sound, USB 2.0,
ATI graphics cards and a shared host folder. It is an experiment and is
not meant for upstream QEMU.

QEMU as a whole is released under the GNU General Public License, version
2. For full licensing details, consult the LICENSE file.

Download
========

Ready-made builds for macOS (Apple silicon and Intel) and 64-bit Windows,
with a GUI for setting up and starting machines:

  https://github.com/cat7/qemu/releases/tag/G4-openbios-2026-11-05

All releases: https://github.com/cat7/qemu/releases

The machine
===========

=====================  ======================================================
Model                  PowerMac3,1 (PowerMac3,6 with more than 4 CPUs)
CPU                    PowerPC 7400 (G4); ``-smp`` up to 32
Memory                 up to 2 GB (``-m``)
Firmware               OpenBIOS, ``G4-openbios`` branch of
                       github.com/cat7/openbios (``openbios-qemu.elf``)
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
=============

- ``openbios-qemu.elf``, the firmware. The release zip has it; to build it
  yourself, use the ``G4-openbios`` branch of github.com/cat7/openbios.
- A Mac OS 9.2 or Mac OS X 10.0 to 10.5 install CD or DVD as an image
  (``.iso``, ``.toast``, ``.cdr``, ``.dmg``). It is not included.
- An empty disk image::

    qemu-img create -f raw macosx.img 20G

- For an ATI card: its Mac ROM image. It is not included.

Building
========

.. code-block:: shell

  mkdir build
  cd build
  ../configure --target-list=ppc-softmmu --enable-cocoa --enable-slirp
  make qemu-system-ppc

Only ``qemu-system-ppc`` is needed. ``--enable-cocoa`` is for macOS; on
Windows (MSYS2 UCRT64) use ``--enable-sdl --enable-gtk`` instead.

Machine options
===============

``-M mac99,option=value,...``

- ``via=pmu|pmu-adb|cuda`` -- the power controller. Use ``pmu``: more than
  one CPU needs a PMU. ``pmu`` gives a USB keyboard and mouse, ``pmu-adb``
  and ``cuda`` ADB ones.
- ``nec-usb=on`` -- adds the NEC USB 2.0 card (Mac OS X 10.3 and later),
  for passing host USB devices through.
- ``decr-cap-ms=N`` -- fires each CPU's decrementer at least every N ms,
  so idle CPUs do not sleep too long.

Installing
==========

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
=======

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
============

- At most 2 GB of memory.
- Two of KeyLargo's three ATA buses are modelled.
- The Radeon 9800 runs only on Mac OS X 10.3 to 10.5. Mac OS 9's
  shared-folder driver does not fit in OpenBIOS beside the Radeon 9800's
  ROM; use the Rage 128 Pro there.
- Mac OS X 10.5 deadlocks above about 16 CPUs.
