===========
QEMU README
===========

QEMU is a generic and open source machine & userspace emulator and
virtualizer.

QEMU is capable of emulating a complete machine in software without any
need for hardware virtualization support. By using dynamic translation,
it achieves very good performance. QEMU can also integrate with the Xen
and KVM hypervisors to provide emulated hardware while allowing the
hypervisor to manage the CPU. With hypervisor support, QEMU can achieve
near native performance for CPUs. When QEMU emulates CPUs directly it is
capable of running operating systems made for one machine (e.g. an ARMv7
board) on a different machine (e.g. an x86_64 PC board).

QEMU is also capable of providing userspace API virtualization for Linux
and BSD kernel interfaces. This allows binaries compiled against one
architecture ABI (e.g. the Linux PPC64 ABI) to be run on a host using a
different architecture ABI (e.g. the Linux x86_64 ABI). This does not
involve any hardware emulation, simply CPU and syscall emulation.

QEMU aims to fit into a variety of use cases. It can be invoked directly
by users wishing to have full control over its behaviour and settings.
It also aims to facilitate integration into higher level management
layers, by providing a stable command line interface and monitor API.
It is commonly invoked indirectly via the libvirt library when using
open source applications such as oVirt, OpenStack and virt-manager.

QEMU as a whole is released under the GNU General Public License,
version 2. For full licensing details, consult the LICENSE file.


Running the Beige Power Mac G3 (this fork)
==========================================

This fork's focus is the ``g3beige`` machine (Beige Power Mac G3
"Gossamer") booted from a real dumped ROM into classic Mac OS. A full
invocation looks like:

.. code-block:: shell

  ./qemu-system-ppc \
    -M g3beige -m 256 \
    -bios /path/to/PowerMacG3v3.ROM \
    -drive file=/path/to/disk.img,format=raw,media=disk \
    -cdrom /path/to/cd.iso \
    -nic user,model=bmac \
    -display cocoa \
    -audiodev coreaudio,id=snd -global awacs.audiodev=snd \
    -plugin contrib/plugins/libgovernor.dylib,mips=100

Machine and firmware
--------------------

* ``-M g3beige -m 256`` -- the Gossamer machine with 256 MB RAM (the
  real board maxes out at 768).
* ``-bios PowerMacG3v3.ROM`` -- a real dumped Beige G3 ROM image.
* ``-plugin contrib/plugins/libgovernor.dylib,mips=100`` -- **required**.
  Caps guest execution speed so the ROM's ``TimeDBRA`` timing
  calibration works; without it the CPU outruns the 1 ms VIA timing
  window, the calibration stores 0, and Mac OS later dies with
  "divide by zero" system errors (Apple's ATAPI CD driver, Open
  Transport AppleTalk). Values up to ~200 MIPS calibrate correctly;
  250 and above do not. See ``contrib/plugins/governor.c`` for the
  full story. Build the plugin with
  ``ninja contrib/plugins/libgovernor.dylib`` (``.so`` on Linux).

Storage and choosing the boot device
------------------------------------

* ``-drive file=...,format=raw,media=disk`` -- IDE hard disk (raw
  HFS/HFS+ image). Repeat for a second disk (becomes the IDE slave).
* ``-cdrom foo.iso`` -- ATAPI CD-ROM.
* Floppy (SWIM3 Superdrive, boot/read/write; raw MFM images only,
  1440K or 720K)::

    -drive if=none,id=fd,file=floppy.img,format=raw -global swim3.drive=fd
* Boot selection works the classic-Mac way, not the QEMU way: the
  real ROM ignores QEMU's ``-boot`` option and scans for a blessed
  System Folder, preferring the PRAM-stored startup device.

  * Surest way to boot a CD (or floppy): attach only that medium and
    no hard disk.
  * With both attached: pick the CD in the guest's Startup Disk
    control panel and reboot -- the choice persists across QEMU
    restarts (CUDA PRAM is saved to disk by this fork).

Networking
----------

* ``-nic user,model=bmac`` -- the onboard BMAC ethernet on slirp NAT:
  the guest gets 10.0.2.15 via DHCP (gateway 10.0.2.2), with outbound
  TCP/UDP forwarded to the host network. Append
  ``,mac=52:54:00:xx:yy:zz`` to pin the MAC address.
* Omitting all network options creates the bmac with the same
  defaults; ``-net none`` removes the device entirely.
* Debug capture:
  ``-nic user,model=bmac,id=n1 -object filter-dump,id=fd,netdev=n1,file=tx.pcap``

ATI FCode ROM (recommended)
---------------------------

Attaching a real ATI Mach64-family Open Firmware FCode ROM dump as the
video card's PCI expansion ROM makes the machine ROM's OF execute it,
which names the device node properly (``ATY,mach64_3DUPro``) and lets
Apple's real ATI display driver (NDRV) bind -- noticeably faster
display output than the generic fallback path:

.. code-block:: shell

  -global ati-mach64-gt.romfile=/path/to/ati_fcode.rom

Verified working with a Mach64 3D Rage "GT" FCode ROM (ATI p/n
109-39200-00, FCode 1.53) -- its FCode probes the emulated chip and
correctly identifies it as family member "GT-C" (Rage Pro). The ROM
image is ATI's copyrighted firmware and is not shipped here; use your
own dump. Without it everything still works via the guest's software
display path.

With the NDRV bound, the card's DDC/EDID (bit-banged over the monitor
sense pins) lets AppleVision and the Monitors control panel read a
synthesized monitor descriptor -- the guest identifies a "Multiple
Scan Display". The advertised resolution can be tuned with the
standard EDID properties, e.g.
``-global ati-mach64-gt.xres=1024 -global ati-mach64-gt.yres=768``.

The emulated GUI (2D) engine covers the full documented data path --
fills, screen-to-screen and host-sourced blits, monochrome expansion
(host, VRAM-source and 8x8 pattern), all 16 bitwise mixes/ROPs, the
plane write mask, color-compare transparency, the Bresenham line
engine, and the Rage Pro ``DP_SET_GUI_ENGINE2`` context macro that
Apple's accelerated NDRV programs every operation through -- so
non-Mac guests whose drivers use the accelerator (Linux ``atyfb``
consoles, XFree86/X.Org ``mach64`` XAA and EXA) are expected to work
too. Still unimplemented (traced as ``ati_mach64_2d_unimp``): 1/4 bpp
engine modes, polygon/trapezoid fills and the generalized rotating
VRAM pattern source. A byte-exact regression suite for all of this
lives in ``contrib/mach64-2d-test``.

Display, audio, control
-----------------------

* ``-display cocoa`` -- native macOS window (includes this fork's
  host-cursor integration). This is the supported backend on macOS:
  ``-display sdl`` currently wedges the main loop / receives no input
  on macOS hosts (unresolved; SDL is only expected to matter for a
  future Windows build, where it is the primary backend).
* ``-audiodev coreaudio,id=snd -global awacs.audiodev=snd`` -- sound
  output through the AWACS model (use ``-audiodev none,id=snd`` for
  silence). On Linux substitute an appropriate ``-audiodev`` backend.
  Do not expect perfection: the boot chime is accurate, but short
  system sounds (at least under classic Mac OS) can still come out
  with audible glitches -- the Sound Manager streams through a
  buffer only a few milliseconds deep, which real hardware services
  in lockstep but an emulator host cannot always keep up with.
* ``-qmp unix:/tmp/g3.sock,server,nowait`` -- optional QMP control
  socket for scripting (screenshots, pausing, memory dumps).

Non-standard behaviours and workarounds
---------------------------------------

Beyond ordinary device emulation, this fork uses a few deliberate
tricks to get classic Mac OS fully usable; know about them before
debugging "odd" behaviour:

* **Host-driven mouse cursor (ATI card).** Classic Mac OS moves its
  cursor from a VBL-queue task (``CrsrVBLTask``) that stops being
  serviced by the guest's interrupt dispatcher partway through boot,
  so the guest never programs the ATI hardware-cursor position
  registers itself. ``hw/display/ati_mach64.c`` therefore tracks the
  *host* pointer position directly and writes ``CUR_HORZ_VERT_POSN``
  on the guest's behalf -- position only; clicks and all other input
  still travel the normal emulated ADB path. If cursor motion works
  but feels "different" from real hardware, this is why.
* **CPU speed governor** (see above) -- a TCG plugin, not machine
  code, so it must be present at every launch; nothing in the binary
  enforces it.
* **Automatic PRAM/NVRAM persistence.** Two backing files are created
  automatically in the working directory when not explicitly
  configured: ``pram.img`` (CUDA PRAM: startup disk choice, network
  config, time zone...) and ``nvram.img`` (Old World Open Firmware
  NVRAM). Delete them for a "PRAM zap"; override the PRAM file with
  ``-global cuda.drive=<file>``.

  **Keep separate state per guest OS.** A Mac OS X boot rewrites the
  Open Firmware variables in these files; a subsequent classic Mac OS
  boot reusing them then hangs in the ROM halfway through the startup
  chime. Launch each guest OS from its own working directory (each
  gets its own ``pram.img``/``nvram.img``), or delete both files when
  switching between classic Mac OS and OS X.


Documentation
=============

Documentation can be found hosted online at
`<https://www.qemu.org/documentation/>`_. The documentation for the
current development version that is available at
`<https://www.qemu.org/docs/master/>`_ is generated from the ``docs/``
folder in the source tree, and is built by `Sphinx
<https://www.sphinx-doc.org/en/master/>`_.


Building
========

QEMU is multi-platform software intended to be buildable on all modern
Linux platforms, OS-X, Win32 (via the Mingw64 toolchain) and a variety
of other UNIX targets. The simple steps to build QEMU are:


.. code-block:: shell

  mkdir build
  cd build
  ../configure
  make

Additional information can also be found online via the QEMU website:

* `<https://wiki.qemu.org/Hosts/Linux>`_
* `<https://wiki.qemu.org/Hosts/Mac>`_
* `<https://wiki.qemu.org/Hosts/W32>`_


Submitting patches
==================

The QEMU source code is maintained under the GIT version control system.

.. code-block:: shell

   git clone https://gitlab.com/qemu-project/qemu.git

When submitting patches, one common approach is to use 'git
format-patch' and/or 'git send-email' to format & send the mail to the
qemu-devel@nongnu.org mailing list. All patches submitted must contain
a 'Signed-off-by' line from the author. Patches should follow the
guidelines set out in the `style section
<https://www.qemu.org/docs/master/devel/style.html>`_ of
the Developers Guide.

Additional information on submitting patches can be found online via
the QEMU website:

* `<https://wiki.qemu.org/Contribute/SubmitAPatch>`_
* `<https://wiki.qemu.org/Contribute/TrivialPatches>`_

The QEMU website is also maintained under source control.

.. code-block:: shell

  git clone https://gitlab.com/qemu-project/qemu-web.git

* `<https://www.qemu.org/2017/02/04/the-new-qemu-website-is-up/>`_

A 'git-publish' utility was created to make above process less
cumbersome, and is highly recommended for making regular contributions,
or even just for sending consecutive patch series revisions. It also
requires a working 'git send-email' setup, and by default doesn't
automate everything, so you may want to go through the above steps
manually for once.

For installation instructions, please go to:

*  `<https://github.com/stefanha/git-publish>`_

The workflow with 'git-publish' is:

.. code-block:: shell

  $ git checkout master -b my-feature
  $ # work on new commits, add your 'Signed-off-by' lines to each
  $ git publish

Your patch series will be sent and tagged as my-feature-v1 if you need to refer
back to it in the future.

Sending v2:

.. code-block:: shell

  $ git checkout my-feature # same topic branch
  $ # making changes to the commits (using 'git rebase', for example)
  $ git publish

Your patch series will be sent with 'v2' tag in the subject and the git tip
will be tagged as my-feature-v2.

Bug reporting
=============

The QEMU project uses GitLab issues to track bugs. Bugs
found when running code built from QEMU git or upstream released sources
should be reported via:

* `<https://gitlab.com/qemu-project/qemu/-/issues>`_

If using QEMU via an operating system vendor pre-built binary package, it
is preferable to report bugs to the vendor's own bug tracker first. If
the bug is also known to affect latest upstream code, it can also be
reported via GitLab.

For additional information on bug reporting consult:

* `<https://wiki.qemu.org/Contribute/ReportABug>`_


ChangeLog
=========

For version history and release notes, please visit
`<https://wiki.qemu.org/ChangeLog/>`_ or look at the git history for
more detailed information.


Contact
=======

The QEMU community can be contacted in a number of ways, with the two
main methods being email and IRC:

* `<mailto:qemu-devel@nongnu.org>`_
* `<https://lists.nongnu.org/mailman/listinfo/qemu-devel>`_
* #qemu on irc.oftc.net

Information on additional methods of contacting the community can be
found online via the QEMU website:

* `<https://wiki.qemu.org/Contribute/StartHere>`_
